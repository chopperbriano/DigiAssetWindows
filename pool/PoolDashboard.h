//
// PoolDashboard - minimal VT100 TUI for the pool server exe.
//
// Deliberately separate from the main DigiAssetWindows ConsoleDashboard so the
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

// Owns the pool server's live console UI: a background thread that repeatedly
// renders a status screen (node counts, verification/probe stats, payout
// ledger totals, uptime) from the injected db/server/verifier, and reads
// single keypresses to drive operator actions - notably the [P] payout
// preview and the [E]/Y guarded batch payout. Holds no state of its own beyond
// a bounded in-memory log buffer and the pending payout amount; all durable
// data lives in PoolDatabase.
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

    // Clear the screen and spawn the refresh thread (idempotent). Call stop()
    // (or destroy) to join it.
    void start();
    void stop();

    // Append a line to the scrolling log area (thread-safe; oldest lines are
    // dropped past MAX_LOG_LINES).
    void addLog(const std::string& line);

    bool quitRequested() const { return _quit.load(); }

private:
    PoolDatabase& _db;
    PoolServer& _server;
    PoolVerifier& _verifier;
    std::string _configPath;
    // Cached pool-wallet spendable balance (DGB) for the status row - refreshed
    // every ~30s so render() doesn't hit DigiByte RPC every frame. -1 = unknown.
    double _cachedWalletBalance{-1.0};
    int64_t _walletBalanceCheckedAt{0};
    std::atomic<bool> _running{false};
    std::atomic<bool> _awaitingPayoutConfirm{false};
    // The exact payout plan (address -> DGB amount) computed at [E] time and
    // executed verbatim on Y confirm, so what's sent matches what the operator
    // was shown (weighted by each node's coverage x reliability). Only touched on
    // the single dashboard thread.
    std::vector<std::pair<std::string, double>> _pendingPayouts;
    std::atomic<bool> _quit{false};
    std::thread _thread;
    std::chrono::system_clock::time_point _startTime;

    std::mutex _logMutex;
    std::deque<std::string> _logLines;
    static constexpr size_t MAX_LOG_LINES = 200;

    int _width = 120;
    int _height = 40;
    void updateConsoleSize();

    // Cached DB counters for render(). The refresh loop calls render() every
    // 500ms; without this it would run ~8 DB queries - including a
    // COUNT(DISTINCT assetId) full scan - twice a second, contending the DB
    // mutex with /permanent. Refreshed at most every 5s. Touched only by the
    // single render thread, so no extra lock is needed. (perf)
    struct CachedCounts {
        unsigned int totalNodes = 0;
        unsigned int activeNodes = 0;   // seen in last 1h
        unsigned int permAssets = 0;
        unsigned int permPages = 0;
        unsigned int verifiedRecent = 0;
        unsigned int failedOut = 0;
        double       paidTotal = 0.0;
        unsigned int paidCount = 0;
    };
    CachedCounts _counts;
    std::chrono::steady_clock::time_point _lastCountsRefresh;
    bool _countsInit = false;
    void refreshCountsIfStale();

    void refreshLoop();
    void render();
    void processInput();
};

#endif // DIGIASSET_POOL_DASHBOARD_H
