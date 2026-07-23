//
// Embedded web server for DigiAsset Core
// Based on web/main.cpp — serves the web UI via Boost Beast HTTP
//
// Implementation of the node's built-in HTTP file server. Runs a single-threaded
// Boost Beast accept loop that answers HTTP GET requests by streaming static
// files (HTML/CSS/JS/JSON/images) from the web/ tree, plus source and RPC-method
// files from src/. Used by the node so operators can open the local dashboard in
// a browser. Also exposes a helper to look up the node's external IP.
//

#include "WebServer.h"
#include "AppMain.h"
#include "Config.h"
#include "CurlHandler.h"
#include "DigiByteCore.h"
#include "Log.h"
#include "NodeStats.h"
#include "Version.h"
#include <jsoncpp/json/value.h>
#include <jsoncpp/json/writer.h>

// Use real Boost Beast headers (not the stub in src/boost/)
// The NuGet Boost include path must come before src/ in CMakeLists
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// ---- Helpers ----------------------------------------------------------------

// True if the named path exists on disk (any type). Used both to locate the
// web root at construction and to resolve request targets to files.
static bool fileExistsLocal(const std::string& fileName) {
    struct stat buffer {};
    return (stat(fileName.c_str(), &buffer) == 0);
}

// Maps a file path's extension to an HTTP Content-Type. Recognises the handful
// of types the web UI serves; anything unknown falls back to text/html.
static std::string getMimeType(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript";
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") return "application/json";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") return "image/png";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg") return "image/svg+xml";
    return "text/html";
}

// ---- WebServer implementation -----------------------------------------------

// Reads the "webport" setting (default 8090) from the config file, then locates
// the web/ and src/ roots by trying candidate relative paths and keeping the
// first whose index.html exists (build-output layout, repo root, or one level
// up). Does not start the server thread.
WebServer::WebServer(const std::string& configFile) {
    Config config(configFile);
    _port = static_cast<unsigned short>(config.getInteger("webport", 8090));

    // Determine paths relative to the executable
    // In a typical layout: exe is in build/src/Release/, web files are in web/
    _webRoot = "../../../web/";
    _srcRoot = "../../../src/";

    // If running from the repo root, adjust
    if (!fileExistsLocal(_webRoot + "index.html")) {
        _webRoot = "web/";
        _srcRoot = "src/";
    }
    if (!fileExistsLocal(_webRoot + "index.html")) {
        _webRoot = "../web/";
        _srcRoot = "../src/";
    }
}

WebServer::~WebServer() {
    stop();
}

// Spawns the serverLoop thread. Returns immediately if already running.
void WebServer::start() {
    if (_running) return;
    _stopRequested = false;
    _thread = std::thread(&WebServer::serverLoop, this);
}

// Requests the accept loop to exit and blocks until the thread joins. The loop
// spends its time blocked in acceptor.accept(), so simply setting the flag isn't
// enough — we also make a throwaway loopback connection to wake that accept()
// call. It then returns, the loop observes _stopRequested, and exits promptly
// instead of hanging until the next real request arrives.
void WebServer::stop() {
    _stopRequested = true;
    if (_running) {
        try {
            net::io_context ioc{1};
            tcp::socket s{ioc};
            tcp::endpoint ep{net::ip::make_address("127.0.0.1"), _port};
            beast::error_code ec;
            s.connect(ep, ec); // best-effort nudge; ignore errors (loop exits on the flag anyway)
        } catch (...) {}
    }
    if (_thread.joinable()) {
        _thread.join();
    }
    _running = false;
}

// Returns the node's public IP, fetching it once from api.ipify.org (5s timeout)
// and caching the result. Trailing whitespace/newlines are trimmed; on any error
// the value is cached as "unknown". Subsequent calls return the cached string.
std::string WebServer::getExternalIP() {
    if (_externalIPFetched) return _externalIP;
    try {
        _externalIP = CurlHandler::get("http://api.ipify.org", 5000);
        // Trim whitespace/newlines
        while (!_externalIP.empty() && (_externalIP.back() == '\n' || _externalIP.back() == '\r' || _externalIP.back() == ' ')) {
            _externalIP.pop_back();
        }
    } catch (...) {
        _externalIP = "unknown";
    }
    _externalIPFetched = true;
    return _externalIP;
}

// Assembles the live node-status snapshot served at /api/status.json. Pulls from
// the same null-safe AppMain singletons the console dashboard reads, so the
// browser dashboard and the terminal dashboard always agree. Every subsystem is
// optional: a not-yet-wired analyzer/db/core reports zero/false rather than
// erroring, so this is safe to call at any point in the process lifetime.
std::string WebServer::statusJson() {
    AppMain* app = AppMain::GetInstance();
    Json::Value root;

    root["product"] = getProductVersionString();
    root["buildVersion"] = getVersionString();
    root["upstream"] = getUpstreamVersionString();

    // ---- Sync -------------------------------------------------------------
    ChainAnalyzer* analyzer = app->getChainAnalyzerIfSet();
    int syncState = analyzer ? analyzer->getSync() : ChainAnalyzer::STOPPED;
    unsigned int syncHeight = analyzer ? analyzer->getSyncHeight() : 0;

    // Chain tip via DigiByte Core is a synchronous RPC; tolerate it being down.
    unsigned int chainTip = 0;
    DigiByteCore* dgb = app->getDigiByteCoreIfSet();
    if (dgb) {
        try { chainTip = static_cast<unsigned int>(dgb->getBlockCount()); } catch (...) {}
    }
    if (chainTip == 0) chainTip = syncHeight; // fallback so progress isn't NaN

    std::string statusText;
    int blocksBehind = 0;
    bool synced = false;
    if (syncState == ChainAnalyzer::SYNCED) { statusText = "Fully Synced"; synced = true; }
    else if (syncState == ChainAnalyzer::STOPPED) { statusText = "Stopped"; }
    else if (syncState == ChainAnalyzer::INITIALIZING) { statusText = "Initializing"; }
    else if (syncState == ChainAnalyzer::REWINDING) { statusText = "Rewinding (fork detected)"; }
    else if (syncState == ChainAnalyzer::BUSY) { statusText = "Optimizing indexes"; }
    else { blocksBehind = -syncState; statusText = "Syncing"; }

    double progress = 0.0;
    if (chainTip > 0 && syncHeight > 0) {
        progress = static_cast<double>(syncHeight) / static_cast<double>(chainTip);
        if (progress > 1.0) progress = 1.0;
    }

    Json::Value sync;
    sync["state"] = syncState;
    sync["status"] = statusText;
    sync["synced"] = synced;
    sync["height"] = static_cast<Json::UInt>(syncHeight);
    sync["chainTip"] = static_cast<Json::UInt>(chainTip);
    sync["blocksBehind"] = blocksBehind;
    sync["progress"] = progress;
    root["sync"] = sync;

    // ---- Assets -----------------------------------------------------------
    Database* db = app->getDatabaseIfSet();
    Json::Value assets;
    assets["count"] = db ? static_cast<Json::UInt64>(db->getAssetCountOnChain()) : 0;
    assets["topIndex"] = db ? static_cast<Json::UInt64>(db->getMaxAssetIndex()) : 0;
    Json::Value recent(Json::arrayValue);
    if (db) {
        try {
            auto last = db->getLastAssetsIssued(8);
            for (const auto& a : last) {
                Json::Value item;
                item["assetIndex"] = static_cast<Json::UInt64>(a.assetIndex);
                item["assetId"] = a.assetId;
                item["height"] = static_cast<Json::UInt>(a.height);
                recent.append(item);
            }
        } catch (...) {}
    }
    assets["recent"] = recent;
    root["assets"] = assets;

    // ---- IPFS / bitswap + coverage (cached in NodeStats by the dashboard) --
    auto snap = NodeStats::instance().snapshot();
    Json::Value ipfs;
    ipfs["connected"] = (app->getIPFSIfSet() != nullptr);
    Json::Value bitswap;
    bitswap["probed"] = snap.bitswapProbed;
    bitswap["available"] = snap.bitswapAvailable;
    bitswap["blocksSent"] = static_cast<Json::UInt64>(snap.blocksSent);
    bitswap["dataSent"] = static_cast<Json::UInt64>(snap.dataSent);
    bitswap["blocksPerMin"] = snap.blocksPerMin;
    ipfs["bitswap"] = bitswap;
    root["ipfs"] = ipfs;

    Json::Value coverage;
    coverage["checked"] = snap.coverageChecked;
    coverage["tracked"] = static_cast<Json::UInt>(snap.coverageTracked);
    coverage["have"] = static_cast<Json::UInt>(snap.coverageHave);
    coverage["missing"] = static_cast<Json::UInt>(
            snap.coverageTracked > snap.coverageHave ? snap.coverageTracked - snap.coverageHave : 0);
    root["coverage"] = coverage;

    // ---- Services ---------------------------------------------------------
    Json::Value services;
    services["digibyteCore"] = (dgb != nullptr);
    services["database"] = (db != nullptr);

    Json::Value rpc;
    rpc["online"] = (app->getRpcServerIfSet() != nullptr);
    unsigned int rpcPort = 14024;
    try { Config cfg("config.cfg"); rpcPort = static_cast<unsigned int>(cfg.getInteger("rpcassetport", 14024)); } catch (...) {}
    rpc["port"] = static_cast<Json::UInt>(rpcPort);
    services["rpc"] = rpc;

    Json::Value web;
    web["port"] = static_cast<Json::UInt>(_port);
    web["running"] = _running.load();
    services["web"] = web;
    root["services"] = services;

    // ---- DigiByte Core: chain / network / wallet (throttled cache) --------
    // Core RPCs are synchronous; cache the result for a few seconds so a 3s
    // browser poll (or several open tabs) doesn't hammer Core.
    {
        Json::Value coreInfo;
        bool doFetch = false;
        {
            std::lock_guard<std::mutex> lk(_coreMutex);
            auto since = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - _lastCoreFetch).count();
            if (dgb && (!_coreCached || since >= 6.0)) {
                doFetch = true;
                _lastCoreFetch = std::chrono::steady_clock::now(); // claim before fetch (race guard)
            } else {
                coreInfo = _coreCache;
            }
        }
        if (doFetch) {
            Json::Value fresh = buildCoreInfo(dgb);
            std::lock_guard<std::mutex> lk(_coreMutex);
            _coreCache = fresh;
            _coreCached = true;
            coreInfo = fresh;
        }
        root["digibyte"] = coreInfo;
    }

    // ---- Pool + payouts (mirrored from the dashboard via NodeStats) -------
    Json::Value pool;
    pool["probed"] = snap.poolProbed;
    pool["reachable"] = snap.poolReachable;
    pool["nodesOnline"] = static_cast<Json::UInt>(snap.poolNodesOnline);
    pool["server"] = snap.poolServer;
    pool["status"] = snap.poolStatus;      // e.g. "Hosting pool files"
    pool["payment"] = snap.paymentStatus;  // e.g. "active" / "registered (no payouts yet)"
    root["pool"] = pool;

    Json::Value payout;
    payout["address"] = snap.payoutAddress;
    payout["balance"] = snap.payoutBalance;
    root["payout"] = payout;

    // ---- Node identity ----------------------------------------------------
    Json::Value node;
    node["externalIP"] = getExternalIP();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - _startTime).count();
    node["uptimeSeconds"] = static_cast<Json::UInt64>(uptime);
    root["node"] = node;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = ""; // compact single-line JSON
    return Json::writeString(wb, root);
}

// Builds the "digibyte" object from DigiByte Core: chain + network + wallet.
// Every RPC is individually guarded so a locked/absent wallet or a Core that
// went down mid-poll yields partial data instead of failing the whole status.
Json::Value WebServer::buildCoreInfo(DigiByteCore* dgb) {
    Json::Value o;
    if (!dgb) return o;
    const Json::Value emptyParams(Json::arrayValue);

    try {
        Json::Value bi = dgb->sendcommand("getblockchaininfo", emptyParams);
        if (bi.isObject()) {
            if (bi.isMember("chain")) o["chain"] = bi["chain"];
            if (bi.isMember("blocks")) o["blocks"] = bi["blocks"];
            if (bi.isMember("headers")) o["headers"] = bi["headers"];
            if (bi.isMember("difficulty")) o["difficulty"] = bi["difficulty"];
            if (bi.isMember("verificationprogress")) o["verificationProgress"] = bi["verificationprogress"];
            if (bi.isMember("size_on_disk")) o["sizeOnDiskBytes"] = bi["size_on_disk"];
            if (bi.isMember("pruned")) o["pruned"] = bi["pruned"];
        }
    } catch (...) {}

    try {
        Json::Value ni = dgb->sendcommand("getnetworkinfo", emptyParams);
        if (ni.isObject()) {
            if (ni.isMember("connections")) o["connections"] = ni["connections"];
            if (ni.isMember("subversion")) o["subversion"] = ni["subversion"];
            if (ni.isMember("protocolversion")) o["protocolVersion"] = ni["protocolversion"];
            if (ni.isMember("version")) o["coreVersion"] = ni["version"];
        }
    } catch (...) {}

    // Wallet is optional (Core may run with no wallet, or a locked/encrypted one).
    Json::Value wallet;
    try {
        Json::Value wi = dgb->sendcommand("getwalletinfo", emptyParams);
        if (wi.isObject() && wi.isMember("balance")) {
            wallet["loaded"] = true;
            wallet["name"] = wi.get("walletname", "");
            wallet["balance"] = wi["balance"];
            wallet["unconfirmed"] = wi.get("unconfirmed_balance", 0);
            wallet["immature"] = wi.get("immature_balance", 0);
            wallet["txCount"] = wi.get("txcount", 0);
            bool encrypted = wi.isMember("unlocked_until");
            wallet["encrypted"] = encrypted;
            // unlocked_until == 0 => encrypted + currently locked; >0 => unlocked.
            wallet["locked"] = encrypted && wi["unlocked_until"].asInt64() == 0;
        } else {
            wallet["loaded"] = false;
        }
    } catch (...) {
        wallet["loaded"] = false;
    }
    o["wallet"] = wallet;

    return o;
}

// Background-thread body. Binds a TCP acceptor on 0.0.0.0:_port and serves
// requests synchronously, one at a time: reject non-GET methods and targets that
// are empty, non-absolute, or contain ".." (path-traversal guard); otherwise map
// the target to a file under _srcRoot (for /src/ and /rpc/ prefixes) or _webRoot,
// appending index.html for directory targets. Serves the file with a
// content-type from getMimeType, or 404 if missing. On any exception (e.g. bind
// failure) it logs a warning and retries the whole bind after 2s, unless a stop
// was requested. Clears _running on exit.
void WebServer::serverLoop() {
    Log* log = Log::GetInstance();
    _running = true;

    while (!_stopRequested) {
        try {
            net::io_context ioc{1};
            tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), _port}}; //loopback only - local docs UI; was 0.0.0.0 (audit low)

            log->addMessage("Web Server listening on port " + std::to_string(_port));

            while (!_stopRequested) {
                tcp::socket socket{ioc};

                // Set a short timeout so we can check _stopRequested periodically
                acceptor.accept(socket);

                // Read request
                beast::flat_buffer buffer;
                http::request<http::string_body> req;
                beast::error_code ec;
                http::read(socket, buffer, req, ec);
                if (ec) continue;

                // Build response
                http::response<http::string_body> res;
                res.version(req.version());
                res.set(http::field::server, BOOST_BEAST_VERSION_STRING);

                if (req.method() != http::verb::get) {
                    res.result(http::status::bad_request);
                    res.set(http::field::content_type, "text/html");
                    res.body() = "Unknown HTTP-method";
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    continue;
                }

                std::string target(req.target());
                if (target.empty() || target[0] != '/' || target.find("..") != std::string::npos) {
                    res.result(http::status::bad_request);
                    res.set(http::field::content_type, "text/html");
                    res.body() = "Illegal request-target";
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    continue;
                }

                // Live node-status API. Loopback-only (the acceptor binds
                // 127.0.0.1), read-only, and never cached so the dashboard can
                // poll it. Matches a leading prefix so a cache-busting query
                // string (/api/status.json?t=123) still resolves here.
                if (target.rfind("/api/status.json", 0) == 0) {
                    std::string json;
                    try {
                        json = statusJson();
                    } catch (const std::exception& e) {
                        json = std::string("{\"error\":\"") + e.what() + "\"}";
                    } catch (...) {
                        json = "{\"error\":\"unknown\"}";
                    }
                    res.result(http::status::ok);
                    res.set(http::field::content_type, "application/json");
                    res.set(http::field::cache_control, "no-store");
                    res.body() = json;
                    res.keep_alive(req.keep_alive());
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    socket.shutdown(tcp::socket::shutdown_send, ec);
                    continue;
                }

                // Resolve file path
                std::string path;
                if (target.substr(0, 5) == "/src/") {
                    path = _srcRoot + target.substr(1); // strip leading /
                } else if (target.substr(0, 5) == "/rpc/") {
                    path = _srcRoot + "RPC/Methods/" + target.substr(5);
                    if (!fileExistsLocal(path)) {
                        path = _webRoot + target.substr(1);
                    }
                } else {
                    path = _webRoot + target.substr(1);
                }
                if (target.back() == '/') {
                    path += "index.html";
                }

                // Read file
                std::ifstream is(path, std::ifstream::binary);
                if (!is) {
                    res.result(http::status::not_found);
                    res.set(http::field::content_type, "text/html");
                    res.body() = "The resource '" + target + "' was not found.";
                    res.prepare_payload();
                    http::write(socket, res, ec);
                    continue;
                }

                std::string content((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
                res.result(http::status::ok);
                res.set(http::field::content_type, getMimeType(path));
                res.body() = content;
                res.keep_alive(req.keep_alive());
                res.prepare_payload();
                http::write(socket, res, ec);

                socket.shutdown(tcp::socket::shutdown_send, ec);
            }
        } catch (const std::exception& e) {
            if (!_stopRequested) {
                log->addMessage(std::string("Web Server error: ") + e.what(), Log::WARNING);
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }
    _running = false;
}
