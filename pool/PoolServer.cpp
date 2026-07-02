#include "PoolServer.h"
#include "PoolDatabase.h"
#include "CurlHandler.h"
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {
    // --- Minimal URL decode -------------------------------------------------
    // Form-encoded body parser needs this. %HH -> byte, '+' -> space.
    std::string urlDecode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            if (c == '+') {
                out += ' ';
            } else if (c == '%' && i + 2 < s.size()) {
                char hi = s[i + 1];
                char lo = s[i + 2];
                auto hex = [](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    return -1;
                };
                int h = hex(hi);
                int l = hex(lo);
                if (h >= 0 && l >= 0) {
                    out += (char) ((h << 4) | l);
                    i += 2;
                } else {
                    out += c;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    // --- Form body parser ---------------------------------------------------
    // "a=1&b=2&c=3" -> {"a":"1","b":"2","c":"3"}. Values that appear more
    // than once: last wins (simple, matches expected client behavior).
    std::map<std::string, std::string> parseFormBody(const std::string& body) {
        std::map<std::string, std::string> out;
        size_t pos = 0;
        while (pos < body.size()) {
            size_t amp = body.find('&', pos);
            std::string pair = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            } else if (!pair.empty()) {
                out[urlDecode(pair)] = "";
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return out;
    }

    // --- Naive JSON field extractor ----------------------------------------
    // The /list endpoint body is a small JSON object with 3-5 known string
    // or numeric fields. We don't need a full JSON parser here — pulling in
    // jsoncpp for the pool exe was pointless for this — so use a targeted
    // regex-free scan. Returns the raw value (unquoted for strings, as-is
    // for numbers), or empty string if not found.
    std::string jsonField(const std::string& body, const std::string& key) {
        std::string needle = "\"" + key + "\"";
        size_t pos = body.find(needle);
        if (pos == std::string::npos) return "";
        pos = body.find(':', pos + needle.size());
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < body.size() && std::isspace((unsigned char) body[pos])) pos++;
        if (pos >= body.size()) return "";

        if (body[pos] == '"') {
            // String value: find closing quote (ignoring escape sequences
            // for our tiny use case — peerIds and payout addresses don't
            // contain quotes or backslashes).
            pos++;
            size_t end = body.find('"', pos);
            if (end == std::string::npos) return "";
            return body.substr(pos, end - pos);
        }
        // Numeric / bool / null: read until separator
        size_t end = pos;
        while (end < body.size() &&
               body[end] != ',' && body[end] != '}' && body[end] != ']' &&
               !std::isspace((unsigned char) body[end])) {
            end++;
        }
        return body.substr(pos, end - pos);
    }

    // --- DigiByte Core JSON-RPC (read-only, for the stats page) ------------
    // Returns the numeric "result" of an RPC call, or a negative sentinel on
    // any failure so callers can tell "0 balance" from "couldn't reach core".
    double rpcNumber(const std::string& rpcUser, const std::string& rpcPass,
                     int rpcPort, const std::string& method,
                     const std::string& paramsJson) {
        if (rpcUser.empty()) return -1.0;
        std::string body = "{\"jsonrpc\":\"1.0\",\"id\":\"poolstats\",\"method\":\"" +
                           method + "\",\"params\":" + paramsJson + "}";
        std::string url = "http://" + rpcUser + ":" + rpcPass + "@127.0.0.1:" +
                          std::to_string(rpcPort);
        std::string resp;
        long status = 0;
        try {
            status = CurlHandler::postJson(url, body, resp, 8000);
        } catch (...) {
            return -1.0;
        }
        if (status < 200 || status >= 300) return -1.0;
        std::string val = jsonField(resp, "result");
        if (val.empty() || val == "null") return -1.0;
        try {
            return std::stod(val);
        } catch (...) {
            return -1.0;
        }
    }

    // Fetch an address's on-chain totals from an Esplora-style explorer API
    // (GET <prefix><address>, as digiexplorer.info serves at /api/address/).
    // Fills receivedDgb (all funded) and balanceDgb (funded - spent) from
    // chain_stats. Returns false on any failure. Used because the treasury
    // address lives in an external wallet the pool node can't query via RPC.
    bool esploraAddress(const std::string& apiPrefix, const std::string& address,
                        double& receivedDgb, double& balanceDgb) {
        if (apiPrefix.empty() || address.empty()) return false;
        std::string body;
        try {
            body = CurlHandler::get(apiPrefix + address, 8000);
        } catch (...) {
            return false;
        }
        // Scope parsing to chain_stats so we don't accidentally read the
        // mempool_stats block, which repeats the same field names.
        size_t cs = body.find("\"chain_stats\"");
        if (cs == std::string::npos) return false;
        std::string chain = body.substr(cs);
        std::string fundedStr = jsonField(chain, "funded_txo_sum");
        if (fundedStr.empty()) return false;
        std::string spentStr = jsonField(chain, "spent_txo_sum");
        try {
            double funded = std::stod(fundedStr);
            double spent = spentStr.empty() ? 0.0 : std::stod(spentStr);
            receivedDgb = funded / 100000000.0;
            balanceDgb = (funded - spent) / 100000000.0;
            return true;
        } catch (...) {
            return false;
        }
    }

    // Minimal JSON string escaper for building the stats response by hand.
    std::string jsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c: s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char) c < 0x20) {
                        char b[8];
                        snprintf(b, sizeof(b), "\\u%04x", (unsigned char) c);
                        out += b;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    // --- HTTP response builder ---------------------------------------------
    std::string buildResponse(int status,
                              const std::string& contentType,
                              const std::string& body) {
        const char* reason = "OK";
        switch (status) {
            case 200: reason = "OK"; break;
            case 400: reason = "Bad Request"; break;
            case 404: reason = "Not Found"; break;
            case 405: reason = "Method Not Allowed"; break;
            case 500: reason = "Internal Server Error"; break;
            default:  reason = "OK"; break;
        }
        std::ostringstream out;
        out << "HTTP/1.1 " << status << " " << reason << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;
        return out.str();
    }
}

PoolServer::PoolServer(PoolDatabase& db, unsigned int port)
    : _db(db),
      _port(port),
      _io(),
      _workGuard(boost::asio::make_work_guard(_io)),
      _acceptor(_io) {
    // _workGuard as a MEMBER, not a local — same fix as RPC::Server in
    // the main exe. Without this the thread pool exits right after the
    // ctor returns.
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), (unsigned short) _port);
    _acceptor.open(endpoint.protocol());
    _acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    _acceptor.bind(endpoint);
    _acceptor.listen();
}

PoolServer::~PoolServer() {
    stop();
}

void PoolServer::start() {
    if (_running.exchange(true)) return;

    // Thread pool for request handling.
    const size_t poolSize = 8;
    for (size_t i = 0; i < poolSize; i++) {
        _threadPool.emplace_back([this]() {
            try { _io.run(); }
            catch (...) {}
        });
    }

    // Dedicated accept thread. Same pattern as RPC::Server.
    _acceptThread = std::thread([this]() { this->acceptLoop(); });
}

void PoolServer::stop() {
    if (!_running.exchange(false)) return;

    // Close the acceptor first so the accept loop's blocking accept()
    // returns with an error and the thread exits cleanly.
    boost::system::error_code ec;
    _acceptor.close(ec);

    _workGuard.reset();
    _io.stop();

    if (_acceptThread.joinable()) _acceptThread.join();
    for (auto& t: _threadPool) {
        if (t.joinable()) t.join();
    }
    _threadPool.clear();
}

void PoolServer::acceptLoop() {
    while (_running.load()) {
        boost::system::error_code ec;
        boost::asio::ip::tcp::socket socket(_io);
        _acceptor.accept(socket, ec);
        if (ec) {
            // Happens on stop() when the acceptor is closed. Exit the loop.
            if (!_running.load()) break;
            continue;
        }
        uint64_t id = ++_requestCount;
        boost::asio::post(_io, [this, s = std::move(socket), id]() mutable {
            this->handleConnection(std::move(s), id);
        });
    }
}

void PoolServer::handleConnection(boost::asio::ip::tcp::socket socket, uint64_t /*id*/) {
    try {
        // Read request headers. HTTP headers are terminated by \r\n\r\n.
        // We read in chunks until we see the terminator or exceed a sanity
        // cap. For the pool server's tiny requests, 16 KB is plenty.
        const size_t MAX_HEADER = 16 * 1024;
        std::string buf;
        buf.reserve(1024);
        char chunk[1024];
        size_t bodyStart = std::string::npos;

        while (buf.size() < MAX_HEADER) {
            boost::system::error_code ec;
            size_t n = socket.read_some(boost::asio::buffer(chunk, sizeof(chunk)), ec);
            if (ec || n == 0) break;
            buf.append(chunk, n);
            size_t found = buf.find("\r\n\r\n");
            if (found != std::string::npos) {
                bodyStart = found + 4;
                break;
            }
        }
        if (bodyStart == std::string::npos) {
            // Malformed or empty request — drop.
            return;
        }

        std::string headers = buf.substr(0, bodyStart - 4);

        // Parse request line: "METHOD /path HTTP/1.1\r\n"
        size_t eol = headers.find("\r\n");
        std::string requestLine = (eol == std::string::npos)
                                          ? headers
                                          : headers.substr(0, eol);
        size_t sp1 = requestLine.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : requestLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        std::string method = requestLine.substr(0, sp1);
        std::string path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

        // Pull Content-Length so we know how much body to wait for.
        size_t contentLength = 0;
        {
            std::string lc;
            lc.reserve(headers.size());
            for (char c: headers) lc += (char) std::tolower((unsigned char) c);
            size_t clPos = lc.find("content-length:");
            if (clPos != std::string::npos) {
                clPos += strlen("content-length:");
                while (clPos < lc.size() && std::isspace((unsigned char) lc[clPos])) clPos++;
                size_t clEnd = lc.find("\r\n", clPos);
                if (clEnd != std::string::npos) {
                    try {
                        contentLength = (size_t) std::stoul(lc.substr(clPos, clEnd - clPos));
                    } catch (...) {}
                }
            }
        }

        // Read the body (part already in buf + any remainder from the socket).
        std::string body = buf.substr(bodyStart);
        while (body.size() < contentLength) {
            boost::system::error_code ec;
            size_t n = socket.read_some(boost::asio::buffer(chunk, sizeof(chunk)), ec);
            if (ec || n == 0) break;
            body.append(chunk, n);
        }
        if (body.size() > contentLength) body.resize(contentLength);

        // Dispatch.
        int status = 200;
        std::string contentType = "application/json; charset=utf-8";
        std::string responseBody;
        handleRequest(method, path, body, status, contentType, responseBody);

        std::string raw = buildResponse(status, contentType, responseBody);
        boost::asio::write(socket, boost::asio::buffer(raw));
    } catch (...) {
        // Best-effort. Drop the connection on any error.
    }
    boost::system::error_code ignored;
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

void PoolServer::handleRequest(const std::string& method,
                               const std::string& path,
                               const std::string& body,
                               int& outStatus,
                               std::string& outContentType,
                               std::string& outBody) {
    outStatus = 200;
    outContentType = "application/json; charset=utf-8";

    // --- GET /permanent/<page>.json ----------------------------------------
    if (method == "GET" && path.rfind("/permanent/", 0) == 0) {
        handlePermanent(path, outStatus, outBody);
        return;
    }

    // --- POST /keepalive ---------------------------------------------------
    if (method == "POST" && path == "/keepalive") {
        handleKeepalive(body, outBody);
        return;
    }

    // --- POST /list/<floor>.json -------------------------------------------
    if (method == "POST" && path.rfind("/list/", 0) == 0) {
        handleList(path, body, outStatus, outBody);
        return;
    }

    // --- GET /nodes.json ---------------------------------------------------
    if (method == "GET" && path == "/nodes.json") {
        handleNodes(outBody);
        return;
    }

    // --- GET /map.json -----------------------------------------------------
    if (method == "GET" && path == "/map.json") {
        handleMap(outBody);
        return;
    }

    // --- GET /bad.json -----------------------------------------------------
    if (method == "GET" && path == "/bad.json") {
        handleBad(outBody);
        return;
    }

    // --- GET /pool/stats.json ----------------------------------------------
    // Public donation/treasury stats for the pool web page.
    if (method == "GET" && path == "/pool/stats.json") {
        handleStats(outBody);
        return;
    }

    // --- Fallback ----------------------------------------------------------
    outStatus = 404;
    outBody = "{\"error\":\"not found\"}";
}

void PoolServer::setWalletInfo(const std::string& donationAddress,
                               const std::string& rpcUser,
                               const std::string& rpcPass,
                               int rpcPort,
                               const std::string& explorerTxPrefix,
                               const std::string& addrApiPrefix) {
    std::lock_guard<std::mutex> lk(_statsMutex);
    _donationAddress = donationAddress;
    _rpcUser = rpcUser;
    _rpcPass = rpcPass;
    _rpcPort = rpcPort;
    _explorerTxPrefix = explorerTxPrefix;
    _addrApiPrefix = addrApiPrefix;
    _statsCacheTime = 0; // force a refresh on next stats request
}

void PoolServer::handleStats(std::string& outBody) {
    // Snapshot the wallet config + refresh the cached balances if stale. The
    // RPC calls are rate-limited to once per 30s so this public endpoint can't
    // be used to hammer DigiByte Core.
    std::string donationAddress, explorerPrefix;
    double available = 0.0, received = 0.0, treasuryBalance = 0.0;
    {
        std::lock_guard<std::mutex> lk(_statsMutex);
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
        if (now - _statsCacheTime > 30) {
            // Local pool wallet balance (what payouts actually draw from).
            if (!_rpcUser.empty()) {
                double bal = rpcNumber(_rpcUser, _rpcPass, _rpcPort, "getbalance", "[]");
                if (bal >= 0) _cachedAvailable = bal;
            }
            // Treasury received + balance from a public explorer, because the
            // donation address lives in an external wallet this node can't
            // query over RPC.
            if (!_donationAddress.empty() && !_addrApiPrefix.empty()) {
                double rec = 0.0, tbal = 0.0;
                if (esploraAddress(_addrApiPrefix, _donationAddress, rec, tbal)) {
                    _cachedReceived = rec;
                    _cachedTreasuryBalance = tbal;
                }
            }
            _statsCacheTime = now;
        }
        donationAddress = _donationAddress;
        explorerPrefix = _explorerTxPrefix;
        available = _cachedAvailable;
        received = _cachedReceived;
        treasuryBalance = _cachedTreasuryBalance;
    }

    double paidToHosts = _db.getPaidTotalDgb();
    unsigned int paidCount = _db.getPaidCount();
    int64_t hourAgo = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count() - 3600;
    unsigned int verifiedNodes = _db.countVerifiedSince(hourAgo);
    unsigned int totalNodes = _db.countTotalNodes();
    auto recent = _db.getRecentPayouts(15);

    std::ostringstream js;
    js.setf(std::ios::fixed);
    js.precision(8);
    js << "{"
       << "\"donationAddress\":\"" << jsonEscape(donationAddress) << "\","
       << "\"payoutsEnabled\":" << (_payoutsEnabled.load() ? "true" : "false") << ","
       << "\"receivedTotal\":" << received << ","
       << "\"treasuryBalance\":" << treasuryBalance << ","
       << "\"available\":" << available << ","
       << "\"paidToHosts\":" << paidToHosts << ","
       << "\"payoutCount\":" << paidCount << ","
       << "\"verifiedNodes\":" << verifiedNodes << ","
       << "\"totalNodes\":" << totalNodes << ","
       << "\"explorerTxPrefix\":\"" << jsonEscape(explorerPrefix) << "\","
       << "\"recentPayouts\":[";
    for (size_t i = 0; i < recent.size(); i++) {
        const auto& r = recent[i];
        if (i) js << ",";
        js << "{\"address\":\"" << jsonEscape(r.payoutAddress) << "\","
           << "\"amount\":" << (r.amountDgbSat / 100000000.0) << ","
           << "\"paidAt\":" << r.paidAt << ","
           << "\"txid\":\"" << jsonEscape(r.txid) << "\"}";
    }
    js << "]}";
    outBody = js.str();
}

void PoolServer::handlePermanent(const std::string& path, int& outStatus, std::string& outBody) {
    // /permanent/<page>.json
    const std::string prefix = "/permanent/";
    const std::string suffix = ".json";
    if (path.size() <= prefix.size() + suffix.size()) {
        outStatus = 400;
        outBody = "{\"error\":\"bad path\"}";
        return;
    }
    std::string pageStr = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    unsigned int page = 0;
    try {
        page = (unsigned int) std::stoul(pageStr);
    } catch (...) {
        outStatus = 400;
        outBody = "{\"error\":\"bad page\"}";
        return;
    }
    outBody = _db.buildPermanentPageJson(page);
}

void PoolServer::handleKeepalive(const std::string& body, std::string& outBody) {
    // The C++ client posts form-encoded: address=...&peerId=...&visible=v&secret=...
    auto form = parseFormBody(body);
    auto addrIt = form.find("address");
    auto peerIt = form.find("peerId");
    if (addrIt != form.end() && peerIt != form.end()
        && !addrIt->second.empty() && !peerIt->second.empty()) {
        try {
            _db.upsertNode(peerIt->second, addrIt->second);
        } catch (...) {}
    }
    // Match mctrivia's server response exactly — existing clients parse it
    // as the expected-ok sentinel. Don't "fix" this string.
    outBody = "{\"error\":\"unsubscribe failed will time out anyways\"}";
}

void PoolServer::handleList(const std::string& path, const std::string& body, int& outStatus, std::string& outBody) {
    // Body is JSON: {"height":N,"version":V,"show":bool,"peerId":"...","payout":"..."}
    std::string peerId = jsonField(body, "peerId");
    std::string payout = jsonField(body, "payout");
    if (!peerId.empty() && !payout.empty()) {
        try {
            _db.upsertNode(peerId, payout);
        } catch (...) {}
    }

    // We don't track an actual height-delta-based work list yet; Phase 1
    // returns an empty changes block. The client's job is to fetch
    // /permanent/<page>.json for the real list; /list is primarily used for
    // payout registration on the mctrivia protocol side.
    //
    // The key field here is `payoutsEnabled`. New-format clients parse this
    // and display "registered (no payouts yet)" instead of "active" when
    // false. Legacy clients ignore the extra field and still see a 200 OK,
    // which means their registration was accepted even if no money flows.
    //
    // `phase` is informational — operators bump it when they enable payouts
    // (Phase 3) so clients have a clean indicator of the pool's maturity.
    (void) path;
    outStatus = 200;
    outBody = std::string("{\"payoutsEnabled\":") +
              (_payoutsEnabled.load() ? "true" : "false") +
              ",\"phase\":" +
              (_payoutsEnabled.load() ? "3" : "1") +
              ",\"changes\":{}}";
}

void PoolServer::handleNodes(std::string& outBody) {
    outBody = _db.buildNodesJson();
}

void PoolServer::handleMap(std::string& outBody) {
    outBody = _db.buildMapJson();
}

void PoolServer::handleBad(std::string& outBody) {
    // Empty bad list for now. Operator can later add entries if we find
    // assets that need to be explicitly rejected.
    outBody = "{\"assets\":[],\"cids\":[]}";
}
