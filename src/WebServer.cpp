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
#include "Config.h"
#include "CurlHandler.h"
#include "Log.h"

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

// Requests the accept loop to exit and blocks until the thread joins. Note the
// loop only observes the stop flag between accepted connections, so this may
// block until the next request arrives.
void WebServer::stop() {
    _stopRequested = true;
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
            tcp::acceptor acceptor{ioc, {net::ip::make_address("0.0.0.0"), _port}};

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
