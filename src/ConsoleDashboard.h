//
// Console Dashboard - in-place TUI for DigiAsset Core
//

#ifndef DIGIASSET_CORE_CONSOLEDASHBOARD_H
#define DIGIASSET_CORE_CONSOLEDASHBOARD_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

class ConsoleDashboard {
public:
    ConsoleDashboard();
    ~ConsoleDashboard();

    // Start/stop the refresh thread
    void start();
    void stop();

    // Called by Log to add a message to the recent-messages area
    void addMessage(const std::string& message);

    // Enable VT100 escape sequences on the Windows console.
    // Call once at the very start of main(), before any output.
    static bool enableVT100();

    // Set callback for quit command
    void setQuitCallback(std::function<void()> cb) { _quitCallback = cb; }

    // Check if quit was requested via keyboard
    bool quitRequested() const { return _quitRequested; }

private:
    // Refresh loop (runs in its own thread)
    void refreshLoop();

    // Process keyboard input (non-blocking)
    void processInput();

    // Render one frame to the console
    void render();

    // Helpers
    static std::string centerText(const std::string& text, int width);
    static std::string padRight(const std::string& text, int width);
    static std::string formatDuration(double seconds);
    static std::string formatNumber(uint64_t n);
    static std::string progressBar(double fraction, int width);

    // State
    std::atomic<bool>       _running{false};
    std::thread             _thread;
    std::mutex              _msgMutex;
    std::deque<std::string> _messages;
    static const size_t     MAX_MESSAGES = 100;

    // For computing blocks/sec from height deltas
    unsigned int _lastHeight = 0;
    std::chrono::steady_clock::time_point _lastTime;
    double _blocksPerSec = 0.0;

    // Payout address and balance (refreshed every 60 seconds)
    std::string _payoutAddress;
    std::string _payoutBalance;
    std::chrono::steady_clock::time_point _lastBalanceTime;
    bool _payoutLoaded = false;
    void loadPayoutInfo();

    // PSP pool status (refreshed every 10 minutes). checkPspRegistration()
    // runs on a detached background thread, so every touch of these fields
    // must hold _pspStatusMutex — otherwise render() races against the write
    // inside the background thread and we get undefined behavior on std::string.
    std::mutex _pspStatusMutex;
    std::string _pspStatus;
    int _pspNodeCount = 0;
    std::chrono::steady_clock::time_point _lastPspCheck;
    void checkPspRegistration();

    // IPFS bitswap stats: are we actually serving content out to peers?
    // pollBitswapStats() POSTs to <ipfspath>stats/bitswap every ~30s from a
    // detached thread. BlocksSent is a monotonic counter — we diff against
    // the previous reading to compute the rate.
    std::mutex _bitswapMutex;
    bool _bitswapProbed = false;
    bool _bitswapAvailable = false; // false if IPFS API unreachable
    uint64_t _bitswapBlocksSent = 0;
    uint64_t _bitswapDataSent = 0;
    uint64_t _bitswapBlocksSentPrev = 0;
    std::chrono::steady_clock::time_point _bitswapPrevTime;
    double _bitswapBlocksPerMin = 0.0;
    std::chrono::steady_clock::time_point _lastBitswapPoll;
    void pollBitswapStats();

    // Permanent-list coverage check: download mctrivia's /permanent/<page>.json
    // pages 0..N, extract every assetId mctrivia tracks, and query the local
    // assets table to count how many we have. A healthy node should have
    // 100% coverage — every PSP-enrolled issuance on-chain should appear in
    // both lists. Missing assetIds indicate a chain-analyzer bug or lost sync.
    std::mutex _coverageMutex;
    bool _coverageChecked = false;
    unsigned int _coverageTrackedCount = 0;   // total assetIds in mctrivia's permanent pages
    unsigned int _coverageHaveCount = 0;      // of those, how many are in our local db
    std::chrono::steady_clock::time_point _lastCoverageCheck;
    void checkPermanentCoverage();

    // Cached asset count (refreshed every 5 seconds)
    uint64_t _assetCount = 0;
    std::chrono::steady_clock::time_point _lastAssetCountTime;

    // Application start time (system_clock for displaying actual time)
    std::chrono::system_clock::time_point _startTime;

    // Keyboard input
    std::atomic<bool>       _quitRequested{false};
    std::function<void()>   _quitCallback;
    bool                    _showDebug = false;

    // Port check results: port -> "Open" / "Closed" / "Checking..." / ""
    std::mutex _portCheckMutex;
    std::map<int, std::string> _portStatus;
    bool _portCheckDone = false;
    void checkPorts();

    // RPC server liveness probe (cached). The AppMain raw pointer can outlive
    // the actual listen socket if the RPC accept loop dies in its detached
    // thread, so we verify by TCP-connecting to the configured rpcassetport.
    std::mutex _rpcProbeMutex;
    bool _rpcProbed = false;
    bool _rpcListening = false;
    unsigned int _rpcProbedPort = 0;
    std::chrono::steady_clock::time_point _lastRpcProbe;
    void probeRpcServer();

    // IPFS announce detection/repair (for NAT'd users who have port forwarded
    // but Kubo's AutoNAT hasn't yet published a direct address in /id)
    std::mutex _ipfsAnnounceMutex;
    bool _ipfsAnnouncedDirectly = false;   // /id lists an address containing our WAN IP
    bool _ipfsPort4001Open = false;         // ifconfig.co reports 4001 reachable
    bool _ipfsAnnounceChecked = false;      // we've completed at least one check
    bool _ipfsAnnounceFixApplied = false;   // F was pressed; poll aggressively until Kubo catches up
    std::string _ipfsAnnounceHint;          // dashboard status line, empty if all good
    std::chrono::steady_clock::time_point _lastIpfsAnnounceCheck;
    void checkIpfsAnnounce();               // background: diagnose state, set hint
    bool applyIpfsAnnounceFix();            // [F] handler: POST to IPFS config API

    // [V] handler: fetch /api/v0/id and emit copy-pasteable
    // `ipfs swarm connect` commands so a known-good remote box (e.g. a Linux
    // node) can confirm our announced multiaddrs are actually dialable.
    void printSwarmConnectCommands();

    // [N] handler: fetch the configured pool's /nodes.json, list the peerIds it
    // reports online, and self-check whether our own node is among them.
    void listPoolNodes();

    // Resolve the configured pool base URL (psp1server) with a sane default and
    // no trailing slash. Shared by checkPspRegistration() and listPoolNodes().
    std::string getConfiguredPoolBase();

    // Console dimensions
    int _width = 80;
    int _height = 25;
    void updateConsoleSize();
};

#endif // DIGIASSET_CORE_CONSOLEDASHBOARD_H
