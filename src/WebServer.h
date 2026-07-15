//
// Embedded web server for DigiAsset Core
// Based on web/main.cpp — serves the web UI via Boost Beast HTTP
//
// Interface for the node's built-in HTTP server. The node (DigiAssetWindows.exe)
// runs one WebServer on a background thread to serve the local web UI (static
// files from web/ and helper resources from src/) so operators can view node
// status and manage assets in a browser. Read-only static file serving; it does
// not expose RPC state directly.
//

#ifndef DIGIASSET_CORE_WEBSERVER_H
#define DIGIASSET_CORE_WEBSERVER_H

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// Owns a background thread that accepts HTTP GET requests and returns static
// files from the resolved web/ and src/ roots. Lifetime-safe: the destructor
// stops the thread. Construction reads the listen port and locates the web
// root; start()/stop() control the accept loop.
class WebServer {
public:
    // Loads webport from the given config file (default 8090) and probes the
    // filesystem to locate the web/ and src/ roots relative to the executable.
    explicit WebServer(const std::string& configFile = "config.cfg");
    // Stops the server thread if still running.
    ~WebServer();

    // Launch the accept loop on a background thread; no-op if already running.
    void start();
    // Signal the loop to stop and join the thread (blocks until it exits).
    void stop();

    unsigned short getPort() const { return _port; }
    bool isRunning() const { return _running; }

    // Get the external IP address (cached, fetched once on first call)
    std::string getExternalIP();

private:
    // Thread body: binds the acceptor and serves requests until stop requested.
    void serverLoop();

    // Builds the live node-status JSON served at /api/status.json. Reads the
    // node's subsystems through AppMain's null-safe getters (same sources the
    // console dashboard uses), so it is safe to call before every subsystem is
    // wired up. Never throws for a missing subsystem; reports zero/false instead.
    std::string statusJson();

    unsigned short _port = 8090;
    std::string _webRoot;
    std::string _srcRoot;
    std::thread _thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _stopRequested{false};

    // Cached external IP
    std::string _externalIP;
    bool _externalIPFetched = false;

    // Process start, captured at construction, for the dashboard uptime figure.
    std::chrono::steady_clock::time_point _startTime = std::chrono::steady_clock::now();
};

#endif // DIGIASSET_CORE_WEBSERVER_H
