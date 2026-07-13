//
// PoolServer.cpp - the pool server's embedded HTTP server (Boost.Asio).
//
// Listens on the configured port with a small accept-thread + worker-pool
// model, parses raw HTTP by hand (no framework), and serves the DigiAsset
// Permanent Storage Pool protocol endpoints:
//   GET  /permanent/<page>.json  - the canonical asset->CID work list
//   POST /keepalive              - node liveness + payout-address registration
//   POST /list/<floor>.json      - mctrivia-protocol registration/list call
//   GET  /nodes.json, /map.json, /bad.json - node discovery + world map data
//   GET  /pool/stats.json        - public treasury/donation/ledger stats
// Node registrations are persisted via PoolDatabase; the registering node's
// real IP is resolved (X-Forwarded-For behind the reverse proxy) so it can be
// geolocated for the map. The anonymous namespace holds the HTTP/JSON/IP
// parsing helpers and the read-only DigiByte Core + explorer lookups used to
// build the stats response.
//

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

    // Case-insensitive lookup of a single HTTP header's value from the raw
    // header block (everything before the blank line). Returns "" if absent.
    std::string getHeaderValue(const std::string& headers, const std::string& name) {
        std::string lc;
        lc.reserve(headers.size());
        for (char c : headers) lc += (char) std::tolower((unsigned char) c);
        std::string key = name;
        for (char& c : key) c = (char) std::tolower((unsigned char) c);
        // Header lines are always preceded by a CRLF (the request line is
        // first), so match "\r\n<key>:". lc and headers share offsets since
        // lowercasing doesn't change length.
        size_t p = lc.find("\r\n" + key + ":");
        if (p == std::string::npos) return "";
        size_t valStart = p + 2 + key.size() + 1;
        size_t valEnd = headers.find("\r\n", valStart);
        std::string val = headers.substr(valStart,
            valEnd == std::string::npos ? std::string::npos : valEnd - valStart);
        size_t a = val.find_first_not_of(" \t");
        size_t b = val.find_last_not_of(" \t");
        if (a == std::string::npos) return "";
        return val.substr(a, b - a + 1);
    }

    // Is this a routable public IP? We only plot public addresses on the map,
    // so a loopback/LAN address (a test node on the same host, or a proxy hop
    // leaking through) never geolocates to a bogus pin.
    bool isPublicIp(const std::string& ip) {
        if (ip.empty()) return false;
        if (ip == "::1" || ip == "0.0.0.0") return false;
        if (ip.rfind("127.", 0) == 0) return false;      // loopback
        if (ip.rfind("10.", 0) == 0) return false;       // RFC1918
        if (ip.rfind("192.168.", 0) == 0) return false;  // RFC1918
        if (ip.rfind("169.254.", 0) == 0) return false;  // link-local
        if (ip.rfind("172.", 0) == 0) {                  // 172.16-31.x = RFC1918
            size_t dot = ip.find('.', 4);
            if (dot != std::string::npos) {
                try {
                    int second = std::stoi(ip.substr(4, dot - 4));
                    if (second >= 16 && second <= 31) return false;
                } catch (...) {}
            }
        }
        // IPv6 unique-local (fc00::/7) and link-local (fe80::/10).
        std::string lc;
        for (char c : ip) lc += (char) std::tolower((unsigned char) c);
        if (lc.rfind("fc", 0) == 0 || lc.rfind("fd", 0) == 0) return false;
        if (lc.rfind("fe80", 0) == 0) return false;
        return true;
    }

    // The pool server's own public IP, looked up once (via ip-api) and cached.
    // A node that registers from loopback/LAN - e.g. one running ON the pool box,
    // which the setup guide recommends - has no public client IP and would be
    // invisible on the map. We plot it at the pool's own location instead.
    std::string serverPublicIp() {
        static std::mutex m;
        static std::string cached;
        static bool tried = false;
        std::lock_guard<std::mutex> lk(m);
        if (tried) return cached;
        tried = true;
        try {
            std::string ip = CurlHandler::get("http://ip-api.com/line/?fields=query", 8000);
            size_t a = ip.find_first_not_of(" \t\r\n");
            size_t b = ip.find_last_not_of(" \t\r\n");
            if (a != std::string::npos) ip = ip.substr(a, b - a + 1); else ip.clear();
            if (isPublicIp(ip)) cached = ip;
        } catch (...) {}
        return cached;
    }

    // Work out the registering node's real IP. Behind the Caddy reverse proxy
    // every connection arrives from 127.0.0.1, so the true client address is in
    // X-Forwarded-For (Caddy sets it) or X-Real-IP; fall back to the socket peer
    // for direct connections. If none is a public IP (a co-located/LAN node),
    // fall back to the pool server's own public IP so it still maps.
    std::string resolveClientIp(const std::string& headers, const std::string& socketPeer) {
        std::string xff = getHeaderValue(headers, "X-Forwarded-For");
        if (!xff.empty()) {
            // "client, proxy1, proxy2" — the left-most entry is the origin.
            std::string first = xff.substr(0, xff.find(','));
            size_t a = first.find_first_not_of(" \t");
            size_t b = first.find_last_not_of(" \t");
            if (a != std::string::npos) first = first.substr(a, b - a + 1);
            if (isPublicIp(first)) return first;
        }
        std::string xrip = getHeaderValue(headers, "X-Real-IP");
        if (isPublicIp(xrip)) return xrip;
        if (isPublicIp(socketPeer)) return socketPeer;
        return serverPublicIp();
    }

    // Geolocate a batch of IPs via ip-api.com (free, no key, server-side only).
    // Returns IP -> JSON fragment ("lat":..,"lon":..,"city":..,"country":..).
    std::map<std::string, std::string> geolocateBatch(const std::vector<std::string>& ips) {
        std::map<std::string, std::string> out;
        if (ips.empty()) return out;
        std::string body = "[";
        for (size_t i = 0; i < ips.size(); i++) {
            if (i) body += ",";
            body += "{\"query\":\"" + ips[i] + "\"}";
        }
        body += "]";
        std::string resp;
        long status = 0;
        try {
            status = CurlHandler::postJson(
                "http://ip-api.com/batch?fields=status,lat,lon,city,country,query",
                body, resp, 12000);
        } catch (...) { return out; }
        if (status < 200 || status >= 300) return out;

        // ip-api returns a flat array of objects; walk them one {..} at a time.
        size_t pos = 0;
        while (true) {
            size_t ob = resp.find('{', pos);
            if (ob == std::string::npos) break;
            size_t cb = resp.find('}', ob);
            if (cb == std::string::npos) break;
            std::string obj = resp.substr(ob, cb - ob + 1);
            pos = cb + 1;
            if (jsonField(obj, "status") != "success") continue;
            std::string q = jsonField(obj, "query");
            std::string lat = jsonField(obj, "lat");
            std::string lon = jsonField(obj, "lon");
            if (q.empty() || lat.empty() || lon.empty()) continue;
            out[q] = "\"lat\":" + lat + ",\"lon\":" + lon +
                     ",\"city\":\"" + jsonEscape(jsonField(obj, "city")) + "\"," +
                     "\"country\":\"" + jsonEscape(jsonField(obj, "country")) + "\"";
        }
        return out;
    }

    // --- HTTP response builder ---------------------------------------------
    // Assemble a complete HTTP/1.1 response (status line, content-type/length,
    // permissive CORS, Connection: close) with the given body.
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

// Open, bind, and listen on the given TCP port immediately (so a bind failure
// surfaces at construction), but don't accept connections until start(). The
// io_context work guard is held as a member so the worker pool doesn't drain
// as soon as the constructor returns.
PoolServer::PoolServer(PoolDatabase& db, unsigned int port)
    : _db(db),
      _port(port),
      _io(),
      _workGuard(boost::asio::make_work_guard(_io)),
      _acceptor(_io) {
    // _workGuard as a MEMBER, not a local — same fix as RPC::Server in
    // the main exe. Without this the thread pool exits right after the
    // ctor returns.
    // Bind LOOPBACK only. All legitimate traffic arrives via localhost - the
    // Caddy reverse proxy (public HTTPS) and any co-located node both connect to
    // 127.0.0.1:<port>. Binding 0.0.0.0 needlessly exposed the pool directly to
    // the internet. (audit MUST-FIX #6/#7)
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), (unsigned short) _port);
    _acceptor.open(endpoint.protocol());
    _acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    _acceptor.bind(endpoint);
    _acceptor.listen();
}

PoolServer::~PoolServer() {
    stop();
}

// Spin up the worker thread pool (running the io_context) and the dedicated
// accept thread. Idempotent — a second call while running is a no-op.
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

// Stop accepting, release the work guard, stop the io_context, and join the
// accept thread and every worker. Idempotent; also called from the destructor.
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

// Accept-thread body: block on accept(), bump the request counter, and post
// each accepted socket to the io_context worker pool for handling. Exits when
// stop() closes the acceptor.
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

// Worker-thread body for one connection: read the HTTP headers (up to a 16 KB
// cap) then the Content-Length body, parse the request line into method/path,
// resolve the client's real IP for geolocation, dispatch through
// handleRequest, and write the response. Best-effort — any error just drops
// the connection. Always shuts down and closes the socket at the end.
void PoolServer::handleConnection(boost::asio::ip::tcp::socket socket, uint64_t /*id*/) {
    try {
        // Hard read/write deadline so a slow client (slowloris / RUDY) can't park
        // a worker thread forever - without this, 8 trickle connections take the
        // whole pool offline. (audit MUST-FIX #6)
        {
            int timeoutMs = 10000;   // 10s; Windows SO_RCVTIMEO optval is ms
            setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeoutMs, sizeof(timeoutMs));
            setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, (const char*) &timeoutMs, sizeof(timeoutMs));
        }
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

        // Cap the body size up front: the pool's requests are tiny, so refuse an
        // oversized Content-Length - an unauthenticated multi-GB body would drive
        // the worker pool to OOM. (audit MUST-FIX #5)
        const size_t MAX_BODY = 64 * 1024;
        if (contentLength > MAX_BODY) {
            boost::system::error_code wec;
            std::string tooBig = "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            boost::asio::write(socket, boost::asio::buffer(tooBig), wec);
            return;
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

        // Resolve the registering node's real IP (X-Forwarded-For behind the
        // proxy, else the socket peer) for the world-map geolocation.
        std::string socketPeer;
        {
            boost::system::error_code pec;
            auto rep = socket.remote_endpoint(pec);
            if (!pec) socketPeer = rep.address().to_string();
        }
        std::string clientIp = resolveClientIp(headers, socketPeer);

        // Dispatch.
        int status = 200;
        std::string contentType = "application/json; charset=utf-8";
        std::string responseBody;
        handleRequest(method, path, body, clientIp, status, contentType, responseBody);

        std::string raw = buildResponse(status, contentType, responseBody);
        boost::asio::write(socket, boost::asio::buffer(raw));
    } catch (...) {
        // Best-effort. Drop the connection on any error.
    }
    boost::system::error_code ignored;
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

// Route a parsed request (method + path) to the matching endpoint handler,
// writing the status, content-type, and body into the out-params. Unmatched
// requests get a 404 JSON error. clientIp is threaded through to the handlers
// that register nodes.
void PoolServer::handleRequest(const std::string& method,
                               const std::string& path,
                               const std::string& body,
                               const std::string& clientIp,
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
        handleKeepalive(body, clientIp, outBody);
        return;
    }

    // --- POST /list/<floor>.json -------------------------------------------
    if (method == "POST" && path.rfind("/list/", 0) == 0) {
        handleList(path, body, clientIp, outStatus, outBody);
        return;
    }

    // --- POST /permanent/add (token-gated marketplace/operator ingestion) --
    if (method == "POST" && path == "/permanent/add") {
        handlePermanentAdd(body, outStatus, outBody);
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

// Store the wallet/treasury config used to build /pool/stats.json (donation
// address, local Core RPC creds, explorer tx-link prefix and address API
// prefix) and invalidate the stats cache so the next request refreshes.
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

// Build the public /pool/stats.json body: pool wallet balance, treasury
// received/balance (via explorer), amount paid to hosts, verified/total node
// counts, the geolocated node array for the world map, and the recent-payouts
// ledger. The external lookups (Core RPC, explorer, ip-api geolocation) are
// cached and refreshed at most once every 30s so this endpoint can't be used
// to hammer those services.
void PoolServer::handleStats(std::string& outBody) {
    // Snapshot the wallet config + refresh the cached balances if stale. The
    // RPC calls are rate-limited to once per 30s so this public endpoint can't
    // be used to hammer DigiByte Core.
    std::string donationAddress, explorerPrefix, nodesJson;
    double available = 0.0, received = 0.0, treasuryBalance = 0.0;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();

    // 1. Fast snapshot of the config + whether a refresh is due. No I/O here.
    std::string rpcUser, rpcPass, donationAddr, addrApiPrefix;
    int rpcPort;
    bool refreshDue;
    {
        std::lock_guard<std::mutex> lk(_statsMutex);
        refreshDue = (now - _statsCacheTime > 30);
        rpcUser = _rpcUser; rpcPass = _rpcPass; rpcPort = _rpcPort;
        donationAddr = _donationAddress; addrApiPrefix = _addrApiPrefix;
    }

    // 2. Refresh the external data — but ONLY if a refresh is due AND no other
    //    thread is already doing it (try_lock). The blocking Core RPC / explorer
    //    / ip-api calls run with _statsMutex RELEASED, so a flood of stats
    //    requests can't pin the worker pool: losers of the try_lock fall through
    //    and serve the last-known-good cache below. (audit M3)
    if (refreshDue && _statsRefreshMutex.try_lock()) {
        std::lock_guard<std::mutex> refreshGuard(_statsRefreshMutex, std::adopt_lock);

        // Local pool wallet balance (what payouts actually draw from).
        double newAvail = 0.0; bool haveAvail = false;
        if (!rpcUser.empty()) {
            double bal = rpcNumber(rpcUser, rpcPass, rpcPort, "getbalance", "[]");
            if (bal >= 0) { newAvail = bal; haveAvail = true; }
        }
        // Treasury received + balance from a public explorer (external wallet).
        double rec = 0.0, tbal = 0.0; bool haveTreasury = false;
        if (!donationAddr.empty() && !addrApiPrefix.empty()) {
            if (esploraAddress(addrApiPrefix, donationAddr, rec, tbal)) haveTreasury = true;
        }
        // Node geolocation for the world map. Read the active IPs + which ones
        // are uncached (under the lock), geolocate the uncached set (no lock),
        // then merge + rebuild the array (under the lock).
        int64_t weekAgo = now - 7 * 24 * 3600;
        auto ipList = _db.getActiveNodeIps(weekAgo);
        std::vector<std::string> ips, uncached;
        {
            std::lock_guard<std::mutex> lk(_statsMutex);
            for (const auto& ip : ipList) {
                if (ip.empty()) continue;
                ips.push_back(ip);
                if (!_geoCache.count(ip) &&
                    std::find(uncached.begin(), uncached.end(), ip) == uncached.end()) {
                    uncached.push_back(ip);
                }
            }
        }
        std::map<std::string, std::string> freshGeo;
        if (!uncached.empty()) freshGeo = geolocateBatch(uncached); // network, no lock

        // Store all results under the data lock.
        {
            std::lock_guard<std::mutex> lk(_statsMutex);
            if (haveAvail) _cachedAvailable = newAvail;
            if (haveTreasury) { _cachedReceived = rec; _cachedTreasuryBalance = tbal; }
            // Rebuild the geo cache keeping ONLY IPs still in the active set, so it
            // cannot grow without bound as node IPs churn over long uptime. Prefer a
            // fresh geolocation this cycle, else carry the previously-cached value.
            std::map<std::string, std::string> nextGeo;
            for (const auto& ip : ips) {
                auto fresh = freshGeo.find(ip);
                if (fresh != freshGeo.end()) { nextGeo[ip] = fresh->second; continue; }
                auto prev = _geoCache.find(ip);
                if (prev != _geoCache.end()) nextGeo[ip] = prev->second;
            }
            _geoCache.swap(nextGeo);
            std::string arr = "[";
            bool firstNode = true;
            for (const auto& ip : ips) {
                auto it = _geoCache.find(ip);
                if (it == _geoCache.end()) continue;
                if (!firstNode) arr += ",";
                firstNode = false;
                arr += "{" + it->second + "}";
            }
            arr += "]";
            _cachedNodesJson = arr;
            _statsCacheTime = now;
        }
    }

    // 3. Serve the cache (fresh or last-known-good). Fast, no I/O.
    {
        std::lock_guard<std::mutex> lk(_statsMutex);
        donationAddress = _donationAddress;
        explorerPrefix = _explorerTxPrefix;
        available = _cachedAvailable;
        received = _cachedReceived;
        treasuryBalance = _cachedTreasuryBalance;
        nodesJson = _cachedNodesJson;
    }

    double paidToHosts = _db.getPaidTotalDgb();
    unsigned int paidCount = _db.getPaidCount();
    int64_t hourAgo = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count() - 3600;
    unsigned int verifiedNodes = _db.countVerifiedSince(hourAgo);
    unsigned int activeNodes = _db.countActiveNodes();   // 7-day, not failed-out (matches the map/nodes)
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
       << "\"activeNodes\":" << activeNodes << ","
       << "\"totalNodes\":" << totalNodes << ","
       << "\"nodes\":" << nodesJson << ","
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

// Parse the page number out of /permanent/<page>.json and serve that page's
// asset->CID work list from the db. Returns 400 on a malformed path or an
// unparseable page number.
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

// POST /permanent/add — token-gated. Adds an asset's CIDs to the permanent
// list (on the current open frontier page) so pool nodes pin + re-serve them.
// Body JSON: {"token":"..","assetId":"La..","txHash":"<txid>","cids":"cid1,cid2"}.
// cids is a comma-separated list (avoids a JSON-array parser). Idempotent via
// INSERT OR IGNORE, so re-posting the same asset is a harmless no-op.
void PoolServer::handlePermanentAdd(const std::string& body, int& outStatus, std::string& outBody) {
    // Disabled unless the operator set a token in pool.cfg.
    if (_ingestToken.empty()) {
        outStatus = 403;
        outBody = "{\"error\":\"ingestion disabled: set pooladmintoken in pool.cfg\"}";
        return;
    }
    std::string token = jsonField(body, "token");
    // Length check first, then a full compare (rejects empty/short guesses).
    if (token.size() != _ingestToken.size() || token != _ingestToken) {
        outStatus = 403;
        outBody = "{\"error\":\"forbidden\"}";
        return;
    }

    std::string assetId = jsonField(body, "assetId");
    std::string txHash = jsonField(body, "txHash");
    std::string cidsRaw = jsonField(body, "cids");
    if (assetId.empty() || txHash.empty() || cidsRaw.empty()) {
        outStatus = 400;
        outBody = "{\"error\":\"assetId, txHash and cids (comma-separated) are required\"}";
        return;
    }

    // Split the comma-separated CID list, trimming whitespace.
    std::vector<std::string> cids;
    size_t start = 0;
    while (start <= cidsRaw.size()) {
        size_t comma = cidsRaw.find(',', start);
        std::string piece = (comma == std::string::npos)
                                ? cidsRaw.substr(start)
                                : cidsRaw.substr(start, comma - start);
        size_t a = piece.find_first_not_of(" \t\r\n");
        size_t b = piece.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) cids.push_back(piece.substr(a, b - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (cids.empty()) {
        outStatus = 400;
        outBody = "{\"error\":\"cids contained no valid entries\"}";
        return;
    }

    unsigned int page = _db.getWritablePage();
    int added = 0;
    for (const auto& cid : cids) {
        try {
            _db.insertPermanentAsset(assetId, txHash, cid, page);
            added++;
        } catch (...) {
            // best-effort per CID; keep going
        }
    }
    outBody = "{\"ok\":true,\"page\":" + std::to_string(page) +
              ",\"added\":" + std::to_string(added) + "}";
}

// Handle a node keepalive: parse the form-encoded body, and if it carries both
// a payout address and peerId, upsert the node (recording clientIp for the
// map). Always replies with mctrivia's exact expected sentinel string, which
// existing clients treat as success.
void PoolServer::handleKeepalive(const std::string& body, const std::string& clientIp, std::string& outBody) {
    // The C++ client posts form-encoded: address=...&peerId=...&visible=v&secret=...
    auto form = parseFormBody(body);
    auto addrIt = form.find("address");
    auto peerIt = form.find("peerId");
    if (addrIt != form.end() && peerIt != form.end()
        && !addrIt->second.empty() && !peerIt->second.empty()) {
        try {
            auto secIt = form.find("secret");
            std::string secret = (secIt != form.end()) ? secIt->second : "";
            _db.upsertNode(peerIt->second, addrIt->second, secret, clientIp);
        } catch (...) {}
    }
    // Match mctrivia's server response exactly — existing clients parse it
    // as the expected-ok sentinel. Don't "fix" this string.
    outBody = "{\"error\":\"unsubscribe failed will time out anyways\"}";
}

// Handle the mctrivia-protocol /list registration call: pull peerId + payout
// from the JSON body and upsert the node (with clientIp). Replies with an
// empty changes block plus payoutsEnabled/phase flags so new-format clients
// can show the pool's payout status while legacy clients still see a 200 OK.
void PoolServer::handleList(const std::string& path, const std::string& body, const std::string& clientIp, int& outStatus, std::string& outBody) {
    // Body is JSON: {"height":N,"version":V,"show":bool,"peerId":"...","payout":"..."}
    std::string peerId = jsonField(body, "peerId");
    std::string payout = jsonField(body, "payout");
    std::string secret = jsonField(body, "secret");
    if (!peerId.empty() && !payout.empty()) {
        try {
            _db.upsertNode(peerId, payout, secret, clientIp);
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
