//
// PoolDashboard - minimal VT100 TUI for the pool server exe.
//
// Deliberately separate from the main DigiAssetCore ConsoleDashboard so the
// pool server is a standalone exe with no link-time dependency on the main
// lib. Reimplements the minimum set of helpers we need (VT100 init, cursor
// home, ERASE_LINE, FG colors, key polling, a log buffer).
//

#ifndef DIGIASSET_POOL_DASHBOARD_H
#define DIGIASSET_POOL_DASHBOARD_H

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class PoolDatabase;
class PoolServer;
class PoolVerifier;

class PoolDashboard {
public:
    // configPath = path to pool.cfg, re-read on each [E] press so
    // the operator can adjust poolspendperperiod without restarting.
    PoolDashboard(PoolDatabase& db, PoolServer& server, PoolVerifier& verifier,
                  const std::string& configPath = "pool.cfg");
    ~PoolDashboard();

    // Enable VT100 escape sequences on the Windows console. Returns true if
    // the terminal is capable — if false, the caller should fall back to
    // printing plain lines.
    static bool enableVT100();

    void start();
    void stop();

    void addLog(const std::string& line);

    bool quitRequested() const { return _quit.load(); }

private:
    PoolDatabase& _db;
    PoolServer& _server;
    PoolVerifier& _verifier;
    std::string _configPath;
    std::atomic<bool> _running{false};
    std::atomic<bool> _awaitingPayoutConfirm{false};
    // Per-node amount (DGB) computed at [E] time and reused on Y confirm, so the
    // amount actually sent matches what the operator was shown. Only touched on
    // the single dashboard thread.
    double _pendingPerNode{0.0};
    std::atomic<bool> _quit{false};
    std::thread _thread;
    std::chrono::system_clock::time_point _startTime;

    std::mutex _logMutex;
    std::deque<std::string> _logLines;
    static constexpr size_t MAX_LOG_LINES = 200;

    int _width = 120;
    int _height = 40;
    void updateConsoleSize();

    void refreshLoop();
    void render();
    void processInput();
};

#endif // DIGIASSET_POOL_DASHBOARD_H
