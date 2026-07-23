//
// Console Dashboard - in-place TUI for DigiAsset Core
//
// Implementation of the node's live terminal dashboard. render() gathers a
// snapshot of node health each ~500ms and repaints the whole screen using VT100
// escape sequences (defined below), while a set of detached worker methods
// (probeRpcServer, pollBitswapStats, checkPermanentCoverage, checkPspRegistration,
// checkIpfsAnnounce, checkPorts, loadPayoutInfo) refresh the underlying data on
// their own cadences and publish it into mutex-guarded fields. Keyboard commands
// are handled by processInput(). Windows-only console/socket code is guarded by
// _WIN32.
//

#include "ConsoleDashboard.h"
#include "AppMain.h"
#include "Config.h"
#include "CurlHandler.h"
#include "IPFS.h"
#include "Log.h"
#include "NodeStats.h"
#include "PermanentStoragePool/pools/mctrivia.h"
#include "Version.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

// ---- VT100 escape helpers --------------------------------------------------
// These work on Windows 10 1511+ once ENABLE_VIRTUAL_TERMINAL_PROCESSING is on.

#define ESC "\033["
#define CURSOR_HOME    ESC "H"
#define ERASE_LINE     ESC "2K"
#define HIDE_CURSOR    ESC "?25l"
#define SHOW_CURSOR    ESC "?25h"
#define BOLD           ESC "1m"
#define DIM            ESC "2m"
#define RESET          ESC "0m"
#define FG_GREEN       ESC "32m"
#define FG_YELLOW      ESC "33m"
#define FG_RED         ESC "31m"
#define FG_CYAN        ESC "36m"
#define FG_WHITE       ESC "37m"
#define FG_BRIGHT_WHITE ESC "97m"

static inline std::string moveTo(int row, int col) {
    return std::string(ESC) + std::to_string(row) + ";" + std::to_string(col) + "H";
}

// ---- ConsoleDashboard implementation ----------------------------------------

ConsoleDashboard::ConsoleDashboard() {
    _lastTime = std::chrono::steady_clock::now();
    _startTime = std::chrono::system_clock::now();
}

ConsoleDashboard::~ConsoleDashboard() {
    stop();
}

/**
 * Turns on ENABLE_VIRTUAL_TERMINAL_PROCESSING for the Windows console so the
 * VT100 escape sequences used by render() are interpreted rather than printed
 * literally. Must be called once at the very start of main(), before any output.
 * Returns true on success (always true on non-Windows, where VT100 is native).
 */
bool ConsoleDashboard::enableVT100() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return false;
    mode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    return SetConsoleMode(hOut, mode) != 0;
#else
    return true; // Unix terminals support VT100 natively
#endif
}

/**
 * Starts the dashboard: clears the screen, paints one frame immediately on the
 * calling thread, then launches the background refreshLoop() thread. No-op if
 * already running.
 */
void ConsoleDashboard::start() {
    if (_running) return;
    _running = true;
#ifdef _WIN32
    SetConsoleTitleA(PRODUCT_NAME);
#endif
    // Clear the screen and render immediately so the user sees the dashboard
    std::cout << ESC "2J" << CURSOR_HOME << std::flush;
    render();
    _thread = std::thread(&ConsoleDashboard::refreshLoop, this);
}

/**
 * Signals the refresh thread to exit, joins it, and restores cursor visibility.
 * Safe to call more than once (destructor also calls it).
 */
void ConsoleDashboard::stop() {
    _running = false;
    if (_thread.joinable()) {
        _thread.join();
    }
    // Restore cursor visibility
    std::cout << SHOW_CURSOR << std::flush;
}

/**
 * Appends a log line to the scrolling message pane, evicting the oldest once the
 * MAX_MESSAGES cap is reached. Thread-safe; called by Log from any thread.
 */
// Current local time as a fixed-width "MM-DD HH:MM:SS " prefix (15 chars) for
// on-screen log lines. The render loop colorizes the level keyword that follows.
static std::string logScreenStamp() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmv, "%m-%d %H:%M:%S");
    return oss.str();
}

void ConsoleDashboard::addMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(_msgMutex);
    _messages.push_back(logScreenStamp() + " " + message);
    while (_messages.size() > MAX_MESSAGES) {
        _messages.pop_front();
    }
}

/**
 * Background thread body: poll the keyboard and repaint the screen every 500ms
 * until _running is cleared.
 */
void ConsoleDashboard::refreshLoop() {
    while (_running) {
        processInput();
        render();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

/**
 * Non-blocking keyboard handler (Windows). Drains all pending keystrokes and
 * dispatches single-key commands: Q quit, L toggle log level, P port check,
 * A list recent assets, F fix IPFS announce, V emit swarm-connect verify
 * commands, N list pool nodes, H/? help menu, and 1-6 for topic detail. The
 * network/DB-heavy actions are spawned on detached threads so input stays
 * responsive.
 */
void ConsoleDashboard::processInput() {
#ifdef _WIN32
    while (_kbhit()) {
        int ch = _getch();
        switch (ch) {
            case 'q':
            case 'Q':
                _quitRequested = true;
                if (_quitCallback) _quitCallback();
                break;
            case 'l':
            case 'L':
                // Toggle log level between INFO and DEBUG
                _showDebug = !_showDebug;
                {
                    Log* log = Log::GetInstance();
                    log->setMinLevelToScreen(_showDebug ? Log::DEBUG : Log::INFO);
                    log->addMessage(_showDebug ? "Log level: DEBUG" : "Log level: INFO");
                }
                break;
            case 'p':
            case 'P':
                // Check published ports
                {
                    Log* log = Log::GetInstance();
                    log->addMessage("Checking published ports...");
                    std::thread([this]() { checkPorts(); }).detach();
                }
                break;
            case 'a':
            case 'A':
                // List recent assets
                {
                    Log* log = Log::GetInstance();
                    Database* adb = AppMain::GetInstance()->getDatabaseIfSet();
                    if (!adb) {
                        log->addMessage("Database not ready");
                        break;
                    }
                    uint64_t total = adb->getAssetCountOnChain();
                    log->addMessage("--- Assets: " + formatNumber(total) + " unique IDs ---");
                    try {
                        auto assets = adb->getLastAssetsIssued(10);
                        if (assets.empty()) {
                            log->addMessage("  No assets found yet");
                        } else {
                            log->addMessage("  Last 10 (index may be higher due to sub-types):");
                            for (const auto& a : assets) {
                                log->addMessage("  #" + std::to_string(a.assetIndex) +
                                    " | " + a.assetId.substr(0, 20) + "..." +
                                    " | block " + formatNumber(a.height));
                            }
                        }
                    } catch (...) {
                        log->addMessage("  Error reading assets");
                    }
                }
                break;
            case 'f':
            case 'F':
                // Fix IPFS Addresses.Announce when we've detected the fixable condition
                {
                    Log* log = Log::GetInstance();
                    log->addMessage("Attempting to fix IPFS announce list...");
                    std::thread([this]() { applyIpfsAnnounceFix(); }).detach();
                }
                break;
            case 'v':
            case 'V':
                // Emit copy-pasteable swarm-connect commands for all announced
                // addresses so the user can verify dial-back from a remote box.
                {
                    Log* log = Log::GetInstance();
                    log->addMessage("Fetching announced multiaddrs from IPFS...");
                    std::thread([this]() { printSwarmConnectCommands(); }).detach();
                }
                break;
            case 'n':
            case 'N':
                // List the nodes the pool sees online and self-check whether
                // OUR node is among them.
                {
                    Log* log = Log::GetInstance();
                    log->addMessage("Fetching pool node list...");
                    std::thread([this]() { listPoolNodes(); }).detach();
                }
                break;
            case 'h':
            case 'H':
            case '?':
                // Show a compact topic menu. User presses 1-6 for details.
                {
                    Log* log = Log::GetInstance();
                    log->addMessage("--- Help: press a number for details ---");
                    log->addMessage("[1] Block Sync   [2] Assets & Coverage   [3] Serving");
                    log->addMessage("[4] Pool & Payment   [5] Services & Config   [6] Keys");
                    log->addMessage("[N] List pool nodes (IP + peerId) and self-check your node");
                }
                break;
            case '1':
                {
                    Log* log = Log::GetInstance();
                    AppMain* app = AppMain::GetInstance();
                    log->addMessage("--- Block Sync ---");
                    ChainAnalyzer* ha = app->getChainAnalyzerIfSet();
                    if (ha) log->addMessage("Height: " + formatNumber(ha->getSyncHeight()));
                    log->addMessage("Shows how far your chain analyzer has processed. 'Fully Synced'");
                    log->addMessage("  means the analyzer and your DigiByte wallet agree on the tip.");
                    log->addMessage("Speed shows ms per block during catch-up; ETA estimates remain.");
                }
                break;
            case '2':
                {
                    Log* log = Log::GetInstance();
                    Database* hdb = AppMain::GetInstance()->getDatabaseIfSet();
                    uint64_t localCount = hdb ? hdb->getAssetCountOnChain() : 0;
                    unsigned int tracked, have;
                    { std::lock_guard<std::mutex> lk(_coverageMutex); tracked = _coverageTrackedCount; have = _coverageHaveCount; }
                    log->addMessage("--- Assets & Coverage ---");
                    log->addMessage("Local: " + formatNumber(localCount) + " unique DigiAsset IDs in your database.");
                    log->addMessage("  Includes NFTs, tokens, domains (DName/DDesk), old pre-PSP assets.");
                    log->addMessage("Tracked: " + std::to_string(tracked) + " = the subset enrolled in the pool's");
                    log->addMessage("  permanent storage list. Always smaller than local (PSP is opt-in).");
                    if (tracked > 0) {
                        char buf[32]; snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * have / (double)tracked);
                        log->addMessage("Coverage: " + std::to_string(have) + "/" + std::to_string(tracked) +
                                        " (" + buf + "). 100% = chain analyzer is healthy.");
                    }
                }
                break;
            case '3':
                {
                    Log* log = Log::GetInstance();
                    uint64_t bs, ds; double r; bool av;
                    { std::lock_guard<std::mutex> lk(_bitswapMutex); av=_bitswapAvailable; bs=_bitswapBlocksSent; ds=_bitswapDataSent; r=_bitswapBlocksPerMin; }
                    log->addMessage("--- Serving ---");
                    if (av) {
                        log->addMessage("Blocks sent: " + formatNumber(bs) + " chunks served to IPFS peers.");
                        char buf[64]; snprintf(buf, sizeof(buf), "%.1f", r);
                        log->addMessage("Rate: " + std::string(buf) + " blocks/min. 0.0 is normal when idle.");
                    } else {
                        log->addMessage("IPFS API unreachable. Is IPFS Desktop running?");
                    }
                    log->addMessage("This proves your node supplies content, not just stores it.");
                }
                break;
            case '4':
                {
                    Log* log = Log::GetInstance();
                    log->addMessage("--- Pool & Payment ---");
                    log->addMessage("Your node pins files the pool server tracks. Others fetch via IPFS.");
                    log->addMessage("Payment shows:  active (green) = DGB is flowing to you");
                    log->addMessage("  registered (yellow) = pool accepted you but payouts not enabled");
                    log->addMessage("  unavailable (red) = pool /list endpoint is broken or unreachable");
                    log->addMessage("  checking (dim) = first probe hasn't finished yet");
                    log->addMessage("Press N to list the pool's online nodes (IP + peerId) + self-check.");
                    log->addMessage("Set psp2server=http://... in config.cfg to use a different pool.");
                }
                break;
            case '5':
                {
                    Log* log = Log::GetInstance();
                    WebServer* hw = AppMain::GetInstance()->getWebServerIfSet();
                    log->addMessage("--- Services & Config ---");
                    if (hw) {
                        log->addMessage("Web UI: http://localhost:" + std::to_string(hw->getPort()) + "/");
                        std::string ip = hw->getExternalIP();
                        if (!ip.empty() && ip != "unknown") log->addMessage("External IP: " + ip);
                    }
                    log->addMessage("RPC: port 14024 (DigiAssetWindows-cli.exe getnodestats)");
                    log->addMessage("Config: config.cfg in the exe's working directory.");
                    log->addMessage("  Key settings: rpcuser, rpcpassword, rpcport, psp2server, psp2payout");
                }
                break;
            case '6':
                {
                    Log* log = Log::GetInstance();
                    log->addMessage("--- Keyboard Shortcuts ---");
                    log->addMessage("Q = Quit       A = Asset count    P = Port check");
                    log->addMessage("L = Log level  V = Verify IPFS    F = Fix IPFS announce");
                    log->addMessage("N = Pool nodes + self-check       H = Help menu  1-6 = Help sections");
                }
                break;
            default:
                break;
        }
    }
#endif
}

/**
 * Refreshes _width/_height from the current console window size (Windows), or a
 * fixed 80x25 elsewhere, clamped to a minimum of 40x15 so the layout never
 * underflows.
 */
void ConsoleDashboard::updateConsoleSize() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        _width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        _height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    _width = 80;
    _height = 25;
#endif
    if (_width < 40) _width = 40;
    if (_height < 15) _height = 15;
}

/**
 * Composes and prints one full dashboard frame. It (1) kicks off any due
 * background probes (PSP status, bitswap, coverage, IPFS announce, RPC) guarding
 * each spawn behind its mutex and a "last checked" timer, (2) snapshots the
 * current sync/asset/service state and computes blocks/sec, ETA and progress,
 * then (3) builds the entire screen into one string (header, aligned status
 * grid, PSP/payment/serving/coverage rows, sync detail, progress bar, log pane,
 * and help bar) and writes it in a single flush to minimize flicker. Runs on the
 * refresh thread, but also once on the main thread from start().
 */
void ConsoleDashboard::render() {
    updateConsoleSize();

    AppMain* app = AppMain::GetInstance();

    // ---- Gather data --------------------------------------------------------
    // Sync state
    int syncState = 0;
    unsigned int syncHeight = 0;
    unsigned int chainTip = 0;
    std::string syncStatusText;
    std::string syncColor = FG_WHITE;

    // Payout balance
    loadPayoutInfo();
    // Run registration check in background to avoid blocking render.
    // Snapshot the elapsed-time decision under the PSP status lock so we don't
    // race against checkPspRegistration()'s writes.
    {
        std::lock_guard<std::mutex> lock(_pspStatusMutex);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - _lastPspCheck).count();
        if (elapsed >= 600.0 || _pspStatus.empty()) {
            std::thread([this]() { checkPspRegistration(); }).detach();
        }
    }

    // Poll IPFS bitswap stats every 30s so the "Serving:" row has a recent
    // block-count and rate to display. The first poll seeds the baseline and
    // reports 0 rate; subsequent polls compute delta.
    //
    // Race note: _lastBitswapPoll is default-initialized to epoch, so on the
    // first render `elapsed` is enormous and we fire immediately. We then
    // update _lastBitswapPoll to `now` BEFORE spawning. That's the full
    // spawn-race guard — do NOT additionally check `!_bitswapProbed`, because
    // the detached thread can take a second or two to actually set _probed,
    // and during that window every 500ms render would spawn a duplicate probe.
    {
        std::lock_guard<std::mutex> lock(_bitswapMutex);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - _lastBitswapPoll).count();
        if (elapsed >= 30.0) {
            _lastBitswapPoll = now;
            std::thread([this]() { pollBitswapStats(); }).detach();
        }
    }

    // Permanent-list coverage check every 10 minutes. Walks all of
    // /permanent/*.json and diffs against the local assets table. Same
    // spawn-race guard notes as bitswap above — _lastCoverageCheck starts at
    // epoch so the first render fires automatically.
    {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - _lastCoverageCheck).count();
        if (elapsed >= 600.0) {
            _lastCoverageCheck = now;
            std::thread([this]() { checkPermanentCoverage(); }).detach();
        }
    }
    // Run IPFS announce diagnosis in background.
    // Poll fast (every 15s) while we're waiting for a user-applied fix to
    // take effect after IPFS Desktop restart; otherwise poll every 10 min.
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - _lastIpfsAnnounceCheck).count();
        double interval = 600.0;
        {
            std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
            if (_ipfsAnnounceFixApplied && !_ipfsAnnouncedDirectly) {
                interval = 15.0; // aggressive polling until Kubo picks up the change
            }
        }
        if (elapsed >= interval || !_ipfsAnnounceChecked) {
            // Only run once WebServer has had time to resolve external IP
            WebServer* ws = app->getWebServerIfSet();
            if (ws && !ws->getExternalIP().empty() && ws->getExternalIP() != "unknown") {
                std::thread([this]() { checkIpfsAnnounce(); }).detach();
            }
        }
    }

    ChainAnalyzer* analyzer = app->getChainAnalyzerIfSet();
    if (analyzer) {
        syncState = analyzer->getSync();
        syncHeight = analyzer->getSyncHeight();
    }

    DigiByteCore* dgb = app->getDigiByteCoreIfSet();

    // Chain tip via getBlockCount is a synchronous RPC; refresh it at most every
    // 3s rather than on every 500ms render. The tip moves ~once/15s anyway. (perf)
    if (dgb) {
        auto sinceTip = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - _lastChainTipTime).count();
        if (_cachedChainTip == 0 || sinceTip >= 3.0) {
            try {
                _cachedChainTip = dgb->getBlockCount();
                _lastChainTipTime = std::chrono::steady_clock::now();
            } catch (...) {}
        }
    }
    chainTip = (_cachedChainTip > 0) ? _cachedChainTip : syncHeight; // fallback while unset

    // Determine sync status text
    if (syncState == ChainAnalyzer::SYNCED) {
        syncStatusText = "Fully Synced";
        syncColor = FG_GREEN;
    } else if (syncState == ChainAnalyzer::STOPPED) {
        syncStatusText = "Stopped";
        syncColor = FG_RED;
    } else if (syncState == ChainAnalyzer::INITIALIZING) {
        syncStatusText = "Initializing...";
        syncColor = FG_YELLOW;
    } else if (syncState == ChainAnalyzer::REWINDING) {
        syncStatusText = "Rewinding (fork detected)";
        syncColor = FG_YELLOW;
    } else if (syncState == ChainAnalyzer::BUSY) {
        syncStatusText = "Optimizing Indexes...";
        syncColor = FG_YELLOW;
    } else {
        // Negative = blocks behind
        int blocksBehind = -syncState;
        syncStatusText = "Syncing (" + formatNumber(blocksBehind) + " blocks behind)";
        syncColor = FG_CYAN;
    }

    // Compute blocks/sec from height delta
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - _lastTime).count();
    if (elapsed >= 1.0 && syncHeight > _lastHeight) {
        _blocksPerSec = (syncHeight - _lastHeight) / elapsed;
        _lastHeight = syncHeight;
        _lastTime = now;
    } else if (syncHeight <= _lastHeight && elapsed >= 3.0) {
        // Not making progress
        _blocksPerSec = 0.0;
        _lastHeight = syncHeight;
        _lastTime = now;
    }

    // ETA
    std::string etaText = "--";
    double progress = 0.0;
    if (chainTip > 0 && syncHeight > 0) {
        progress = (double)syncHeight / (double)chainTip;
        if (progress > 1.0) progress = 1.0;
    }
    if (syncState < 0 && _blocksPerSec > 0.01) {
        int blocksBehind = -syncState;
        double etaSec = blocksBehind / _blocksPerSec;
        etaText = formatDuration(etaSec);
    } else if (syncState == ChainAnalyzer::SYNCED) {
        etaText = "Complete";
    }

    // Asset count (query at most once every 5 seconds to avoid DB load)
    Database* db = app->getDatabaseIfSet();
    if (db && elapsed >= 1.0) {
        auto timeSinceAssetQuery = std::chrono::duration<double>(now - _lastAssetCountTime).count();
        if (timeSinceAssetQuery >= 5.0 || _assetCount == 0) {
            try {
                _assetCount = db->getAssetCountOnChain();
                _lastAssetCountTime = now;
            } catch (...) {}
        }
    }

    // Connection status
    bool dgbOnline = (dgb != nullptr);
    bool dbOnline = (db != nullptr);
    bool ipfsOnline = (app->getIPFSIfSet() != nullptr);

    // RPC: probe the actual listen socket. AppMain stores a raw pointer to
    // RPC::Server that becomes dangling if the detached accept-loop thread
    // dies, so trusting getRpcServerIfSet() can show a stale port for a
    // service that isn't actually listening.
    //
    // Probe cadence is state-dependent: the first render of the dashboard
    // happens on main thread at ConsoleDashboard::start(), ~1s BEFORE main.cpp
    // constructs RPC::Server. If we probed once and slept 30s, the user would
    // stare at "Off" for 30s after a fresh start even though the server came
    // up within the first second. So: if we haven't found it yet, re-probe
    // every 2s. Only stretch to 30s once we've seen a positive result.
    // Update _lastRpcProbe BEFORE spawning, otherwise every render in the
    // ~1ms window before the probe thread finishes will spawn a duplicate.
    bool rpcProbed;
    bool rpcOnline;
    unsigned int rpcPort;
    {
        std::lock_guard<std::mutex> lock(_rpcProbeMutex);
        auto rnow = std::chrono::steady_clock::now();
        auto rElapsed = std::chrono::duration<double>(rnow - _lastRpcProbe).count();
        double probeInterval = _rpcListening ? 30.0 : 2.0;
        if (rElapsed >= probeInterval || !_rpcProbed) {
            _lastRpcProbe = rnow;
            std::thread([this]() { probeRpcServer(); }).detach();
        }
        rpcProbed = _rpcProbed;
        rpcOnline = _rpcListening;
        rpcPort = _rpcProbedPort;
    }
    WebServer* webServer = app->getWebServerIfSet();
    bool webOnline = (webServer != nullptr && webServer->isRunning());
    unsigned short webPort = 0;
    std::string externalIP;
    if (webServer) {
        webPort = webServer->getPort();
        externalIP = webServer->getExternalIP();
    }

    // ---- Build output -------------------------------------------------------
    std::ostringstream out;
    int w = _width;
    int totalRows = 0; // track exactly how many rows we emit

    // Hide cursor + home
    out << HIDE_CURSOR << CURSOR_HOME;

    // Row 1-2: Header
    std::string title = getProductVersionString() + "  (upstream " + getUpstreamVersionString() + ")";
    out << BOLD << FG_BRIGHT_WHITE << ERASE_LINE << centerText(title, w) << RESET << "\n"; totalRows++;
    out << ERASE_LINE << std::string(w, '-') << "\n"; totalRows++;

    // ---- Aligned two-column header ----------------------------------------
    // Layout (visible columns, ignoring ANSI escape codes):
    //
    //   col 0-1   left margin (2 spaces)
    //   col 2-17  col1 label (16 chars: label + colon + spaces)
    //   col 18-31 col1 value (14 chars, padded to fixed width)
    //   col 32-45 col2 label (14 chars: label + colon + spaces)
    //   col 46-?  col2 value (free width, end of line)
    //
    // For long-value rows (Web UI, Payout, Balance, PSP Pool), col1 takes the
    // full row and the col2 cell is omitted.
    //
    // The cell helper pads invisible-length-correct, so ANSI color codes don't
    // throw off column alignment.
    const int COL1_LABEL_W = 16;
    const int COL1_VALUE_W = 14;
    const int COL2_LABEL_W = 14;

    auto cell = [](const std::string& label, const std::string& value, const char* color,
                   int labelW, int valueW) -> std::string {
        std::string lp = label + ":";
        if ((int)lp.size() < labelW) lp += std::string(labelW - lp.size(), ' ');
        std::string result = lp;
        if (color) result += color;
        result += value;
        result += RESET;
        int pad = valueW - (int)value.size();
        if (pad > 0) result += std::string(pad, ' ');
        return result;
    };

    // Row: DigiByte Core | Database
    out << ERASE_LINE << "  "
        << cell("DigiByte Core", dgbOnline ? "Online" : "Offline",
                dgbOnline ? FG_GREEN : FG_RED, COL1_LABEL_W, COL1_VALUE_W)
        << cell("Database", dbOnline ? "Ready" : "N/A",
                dbOnline ? FG_GREEN : FG_RED, COL2_LABEL_W, 0)
        << "\n"; totalRows++;

    // Row: IPFS | RPC Server
    {
        // Tri-state: not-probed-yet (Checking, cyan), probed-and-off (Off, red),
        // probed-and-listening (Port N, green). Without the "not probed yet"
        // case, the first render on a fresh start shows a red "Off" because the
        // probe spawned but hasn't completed yet.
        std::string rpcVal;
        const char* rpcColor;
        if (!rpcProbed) {
            rpcVal = "Checking...";
            rpcColor = FG_CYAN;
        } else if (rpcOnline) {
            rpcVal = "Port " + std::to_string(rpcPort);
            rpcColor = FG_GREEN;
        } else {
            rpcVal = "Off";
            rpcColor = FG_RED;
        }
        out << ERASE_LINE << "  "
            << cell("IPFS", ipfsOnline ? "Connected" : "N/A",
                    ipfsOnline ? FG_GREEN : FG_YELLOW, COL1_LABEL_W, COL1_VALUE_W)
            << cell("RPC Server", rpcVal, rpcColor, COL2_LABEL_W, 0)
            << "\n"; totalRows++;
    }

    // Row: Web Server | External IP
    {
        std::string webVal = webOnline ? ("Port " + std::to_string(webPort)) : std::string("Off");
        out << ERASE_LINE << "  "
            << cell("Web Server", webVal,
                    webOnline ? FG_GREEN : FG_RED, COL1_LABEL_W, COL1_VALUE_W);
        if (!externalIP.empty() && externalIP != "unknown") {
            out << cell("External IP", externalIP, FG_BRIGHT_WHITE, COL2_LABEL_W, 0);
        }
        out << "\n"; totalRows++;
    }

    // Row: Web UI (single column, full row)
    if (webOnline) {
        out << ERASE_LINE << "  "
            << cell("Web UI", "http://localhost:" + std::to_string(webPort) + "/",
                    FG_CYAN, COL1_LABEL_W, 0)
            << "\n"; totalRows++;
    }

    // Row: Payout address (single column, full row)
    if (!_payoutAddress.empty()) {
        out << ERASE_LINE << "  "
            << cell("Payout", _payoutAddress, FG_BRIGHT_WHITE, COL1_LABEL_W, 0)
            << "\n"; totalRows++;
    }

    // Row: Balance (single column with DIM "(updates every 4h)" suffix)
    if (!_payoutBalance.empty()) {
        std::string lp = std::string("Balance:") + std::string(COL1_LABEL_W - 8, ' ');
        out << ERASE_LINE << "  " << lp
            << FG_GREEN << _payoutBalance << RESET
            << DIM << " (updates every 4h)" << RESET
            << "\n"; totalRows++;
    }

    // Row: PSP Pool status.
    //
    // This pulls from THREE independent signals now, not one:
    //   1. map.json fetch succeeds  -> server is up on the static-file side
    //   2. mctrivia::getPermanentFetchHealth()  -> /permanent/<page>.json works,
    //      we're actively pinning canonical content for the pool
    //   3. mctrivia::getRegistrationHealth()    -> /list/<floor>.json POST works,
    //      meaning the legacy registration endpoint responds. Registration +
    //      payouts are handled by the pool server (psp1server) in this fork, so
    //      this signal just drives a dashboard indicator.
    //
    // See memory/project_psp_payment_diagnosis.md for the full story.
    {
        // Null-safe: render() runs once on main thread during ConsoleDashboard::start()
        // BEFORE main.cpp's PSP list construction, so we MUST tolerate a null list here.
        auto* pspList = AppMain::GetInstance()->getPermanentStoragePoolListIfSet();
        // Follow whichever networked pool this node actually joined (psp2 DigiStamp
        // on new nodes, psp1 mctrivia on legacy) - NOT a hardcoded index, or a new
        // node (on psp2, with psp1 unsubscribed) would show "Starting up" forever.
        auto* poolBase = pspList ? pspList->getActiveNetworkedPool() : nullptr;
        auto* pool = dynamic_cast<mctrivia*>(poolBase);

        // Read the mctrivia pool's health signals ONCE per render. Each getter
        // takes the health mutex; reading twice would double the lock overhead
        // and risk rendering two inconsistent states if the fetcher thread
        // mutates between reads.
        mctrivia::Health permHealth = mctrivia::Health::Unknown;
        mctrivia::Health regHealth = mctrivia::Health::Unknown;
        bool payoutsEnabled = false;
        if (pool) {
            permHealth = pool->getPermanentFetchHealth();
            regHealth = pool->getRegistrationHealth();
            payoutsEnabled = pool->getPayoutsEnabled();
        }

        // Snapshot the server-up signal + node count under the lock. The
        // background checkPspRegistration() thread writes _pspStatus and
        // _pspNodeCount, so naked reads here would race.
        bool serverUp;
        int pspNodeCountSnapshot;
        {
            std::lock_guard<std::mutex> lock(_pspStatusMutex);
            serverUp = !_pspStatus.empty() && _pspStatus.find("unreachable") == std::string::npos;
            pspNodeCountSnapshot = _pspNodeCount;
        }

        // Compose the main status line. Wording deliberately avoids jargon
        // ("pinning", "/list", HTTP codes) — a Windows user running this exe
        // should be able to read it and understand whether they're doing
        // something useful and whether they're getting paid.
        std::string statusText;
        const char* statusColor = FG_YELLOW;

        if (!serverUp) {
            statusText = "Pool unreachable";
            statusColor = FG_RED;
        } else if (!pool) {
            // Shouldn't happen in practice, but degrade gracefully if pool 1 is
            // disabled or something is miswired in the PSP list.
            statusText = "Pool reachable";
            statusColor = FG_GREEN;
        } else if (permHealth == mctrivia::Health::Ok && regHealth == mctrivia::Health::Ok) {
            statusText = "Hosting pool files";
            statusColor = FG_GREEN;
        } else if (permHealth == mctrivia::Health::Ok) {
            // Normal current state: we can fetch the canonical list and
            // pin content, but the payment registration endpoint is broken.
            statusText = "Hosting pool files (unpaid)";
            statusColor = FG_YELLOW;
        } else if (permHealth == mctrivia::Health::Unknown && regHealth == mctrivia::Health::Unknown) {
            statusText = "Starting up...";
            statusColor = FG_CYAN;
        } else {
            statusText = "Pool degraded";
            statusColor = FG_YELLOW;
        }

        std::string lp = std::string("PSP Pool:") + std::string(COL1_LABEL_W - 9, ' ');
        out << ERASE_LINE << "  " << lp
            << statusColor << statusText << RESET;
        if (pspNodeCountSnapshot > 0) {
            out << "    Network: " << FG_BRIGHT_WHITE << pspNodeCountSnapshot
                << RESET << DIM << " nodes online" << RESET;
        }
        out << "\n"; totalRows++;

        // Row: Payment. Explicitly tells the user the current economic reality
        // of running this node so nobody thinks they're accruing DGB silently.
        //
        // Four states:
        //   green  active                       - pool registered AND payoutsEnabled=true
        //   yellow registered (no payouts yet)  - pool registered, payoutsEnabled=false
        //                                         (Phase 1 local pool, or legacy server
        //                                         that doesn't send the field)
        //   red    unavailable                  - /list registration probe didn't respond
        //   dim    checking...                  - not probed yet
        std::string paymentPlain = "checking...";
        if (pool) {
            std::string lp2 = std::string("Payment:") + std::string(COL1_LABEL_W - 8, ' ');
            out << ERASE_LINE << "  " << lp2;
            if (regHealth == mctrivia::Health::Ok) {
                if (payoutsEnabled) {
                    paymentPlain = "active";
                    out << FG_GREEN << "active" << RESET;
                } else {
                    paymentPlain = "registered (no payouts yet)";
                    out << FG_YELLOW << "registered (no payouts yet)" << RESET
                        << DIM << " - pool operator has not enabled payouts" << RESET;
                }
            } else if (regHealth == mctrivia::Health::Broken) {
                paymentPlain = "unavailable";
                out << FG_RED << "unavailable" << RESET
                    << DIM << " - pool payment service offline" << RESET;
            } else {
                out << DIM << "checking..." << RESET;
            }
            out << "\n"; totalRows++;
        }

        // Mirror the plain hosting + payment status into NodeStats for the web
        // console (statusText was computed above from the same pool health).
        NodeStats::instance().setPoolStatus(statusText, paymentPlain);
    }

    // Row: Serving. Proves to the operator that blocks are actively flowing
    // out to peers via IPFS bitswap. BlocksSent is a kubo monotonic counter;
    // we diff against the previous snapshot to compute the rate.
    {
        bool available;
        bool probed;
        uint64_t blocksSent;
        uint64_t dataSent;
        double rate;
        {
            std::lock_guard<std::mutex> lock(_bitswapMutex);
            available = _bitswapAvailable;
            probed = _bitswapProbed;
            blocksSent = _bitswapBlocksSent;
            dataSent = _bitswapDataSent;
            rate = _bitswapBlocksPerMin;
        }

        std::string lp3 = std::string("Serving:") + std::string(COL1_LABEL_W - 8, ' ');
        out << ERASE_LINE << "  " << lp3;
        if (!probed) {
            out << FG_CYAN << "checking..." << RESET;
        } else if (!available) {
            out << FG_YELLOW << "IPFS API unreachable" << RESET;
        } else {
            // Format DataSent as KB/MB/GB.
            auto formatBytes = [](uint64_t b) -> std::string {
                char buf[64];
                if (b < 1024ull) { snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b); }
                else if (b < 1024ull * 1024) { snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0); }
                else if (b < 1024ull * 1024 * 1024) { snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0)); }
                else { snprintf(buf, sizeof(buf), "%.2f GB", b / (1024.0 * 1024.0 * 1024.0)); }
                return std::string(buf);
            };

            const char* color = (blocksSent > 0) ? FG_GREEN : FG_YELLOW;
            out << color << formatNumber(blocksSent) << " blocks sent" << RESET;
            char rateBuf[64];
            snprintf(rateBuf, sizeof(rateBuf), " (%.1f/min, %s)",
                     rate, formatBytes(dataSent).c_str());
            out << DIM << rateBuf << RESET;
        }
        out << "\n"; totalRows++;
    }

    // Row: Asset index. Shows local asset count + coverage vs the subset of
    // assets mctrivia's permanent list tracks. 100% = chain analyzer is
    // definitely detecting every PSP-enrolled issuance on-chain, which is
    // strong evidence the analyzer is working end-to-end.
    {
        bool checked;
        unsigned int tracked;
        unsigned int have;
        {
            std::lock_guard<std::mutex> lock(_coverageMutex);
            checked = _coverageChecked;
            tracked = _coverageTrackedCount;
            have = _coverageHaveCount;
        }

        std::string lp4 = std::string("Asset index:") + std::string(COL1_LABEL_W - 12, ' ');
        out << ERASE_LINE << "  " << lp4
            << FG_BRIGHT_WHITE << formatNumber(_assetCount) << " local" << RESET;
        if (checked && tracked > 0) {
            double pct = 100.0 * (double)have / (double)tracked;
            char covBuf[96];
            if (syncState != ChainAnalyzer::SYNCED) {
                // Still catching up: "coverage" is really "how much of the chain
                // we've decoded", which climbs as we sync. Show it calmly, not as
                // an alarming red 0% (nothing is wrong). See PERMANENT-STORAGE.md.
                snprintf(covBuf, sizeof(covBuf), " / %u tracked / %.1f%% decoded (syncing)",
                         tracked, pct);
                out << DIM << covBuf << RESET;
            } else {
                const char* cvrColor = (have == tracked) ? FG_GREEN :
                                       (pct >= 99.0)     ? FG_YELLOW : FG_RED;
                snprintf(covBuf, sizeof(covBuf), " / %u tracked / %.1f%% coverage",
                         tracked, pct);
                out << cvrColor << covBuf << RESET;
            }
        } else if (!checked) {
            out << DIM << " / checking coverage..." << RESET;
        }
        out << "\n"; totalRows++;
    }

    // Row: IPFS announce hint (conditional, only when there's actionable info)
    {
        std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
        if (!_ipfsAnnounceHint.empty()) {
            std::string lp = std::string("IPFS:") + std::string(COL1_LABEL_W - 5, ' ');
            out << ERASE_LINE << "  " << lp
                << FG_YELLOW << _ipfsAnnounceHint << RESET << "\n";
            totalRows++;
        }
    }

    // Row: Time and Uptime
    {
        auto currentTime = std::chrono::system_clock::now();
        auto time_t_val = std::chrono::system_clock::to_time_t(currentTime);
        std::tm timeinfo_buffer{};
        std::tm* timeinfo = &timeinfo_buffer;
#ifdef _WIN32
        localtime_s(&timeinfo_buffer, &time_t_val);
#else
        timeinfo = std::localtime(&time_t_val);
#endif
        std::ostringstream timeOss;
        timeOss << std::put_time(timeinfo, "%H:%M:%S");
        std::string timeStr = timeOss.str();

        // Calculate uptime
        auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(currentTime - _startTime).count();
        std::string uptimeStr = formatDuration((double)uptime_seconds);

        out << ERASE_LINE << "  "
            << cell("Time", timeStr, FG_BRIGHT_WHITE, COL1_LABEL_W, COL1_VALUE_W)
            << cell("Uptime", uptimeStr, FG_BRIGHT_WHITE, COL2_LABEL_W, 0)
            << "\n"; totalRows++;
    }

    // Row 5: separator
    out << ERASE_LINE << std::string(w, '-') << "\n"; totalRows++;

    // Row 6: Sync Status
    out << BOLD << ERASE_LINE << " Sync: " << syncColor << syncStatusText << RESET << "\n"; totalRows++;

    // Row 7: Height
    out << ERASE_LINE << "  Block:   " << FG_BRIGHT_WHITE << formatNumber(syncHeight) << RESET;
    if (chainTip > 0) {
        out << " / " << formatNumber(chainTip);
    }
    out << "\n"; totalRows++;

    // Row 8: Speed & ETA
    out << ERASE_LINE << "  Speed:   ";
    if (_blocksPerSec >= 0.01) {
        out << FG_BRIGHT_WHITE;
        if (_blocksPerSec >= 1.0) {
            out << std::fixed << std::setprecision(1) << _blocksPerSec << " blocks/sec";
        } else {
            double secPerBlock = 1.0 / _blocksPerSec;
            out << std::fixed << std::setprecision(0) << secPerBlock << " sec/block";
        }
        out << RESET;
    } else {
        out << DIM << "--" << RESET;
    }
    out << "    ETA: " << FG_BRIGHT_WHITE << etaText << RESET << "\n"; totalRows++;

    // Row 9: Assets
    out << ERASE_LINE << "  Assets:  " << FG_BRIGHT_WHITE;
    if (_assetCount > 0) {
        out << formatNumber(_assetCount);
    } else {
        out << DIM << "--";
    }
    out << RESET << "\n"; totalRows++;

    // Row 10: Progress bar
    int barWidth = w - 18;
    if (barWidth < 10) barWidth = 10;
    out << ERASE_LINE << "  " << progressBar(progress, barWidth);
    out << " " << std::fixed << std::setprecision(1) << (progress * 100.0) << "%\n"; totalRows++;

    // Row 11: separator
    out << ERASE_LINE << std::string(w, '-') << "\n"; totalRows++;

    // Row 12: Messages header
    out << BOLD << ERASE_LINE << " Log:" << RESET << "\n"; totalRows++;

    // Remaining rows: log messages — reserve 1 row for help bar at bottom
    int logRows = _height - totalRows - 2; // -1 help bar, -1 to avoid scrolling
    if (logRows < 1) logRows = 1;

    {
        std::lock_guard<std::mutex> lock(_msgMutex);
        int startIdx = 0;
        if ((int)_messages.size() > logRows) {
            startIdx = (int)_messages.size() - logRows;
        }
        int printed = 0;
        for (int i = startIdx; i < (int)_messages.size() && printed < logRows; ++i, ++printed) {
            out << ERASE_LINE << "  ";
            const std::string& msg = _messages[i];
            // Colorize by level keyword, which follows the fixed-width
            // "MM-DD HH:MM:SS " timestamp prefix (15 chars).
            const size_t TS = 15;
            auto lvlIs = [&](const char* s, size_t n) {
                return msg.size() >= TS + n && msg.compare(TS, n, s) == 0;
            };
            if (lvlIs("CRITICAL", 8)) {
                out << FG_RED;
            } else if (lvlIs("WARNING", 7)) {
                out << FG_YELLOW;
            } else if (lvlIs("ERROR", 5)) {
                out << FG_RED;
            } else if (lvlIs("DEBUG", 5)) {
                out << DIM;
            } else {
                out << FG_WHITE;
            }
            // Truncate to fit console width
            int maxLen = w - 3;
            if ((int)msg.size() > maxLen && maxLen > 3) {
                out << msg.substr(0, maxLen - 3) << "...";
            } else {
                out << msg;
            }
            out << RESET << "\n";
        }
        // Fill remaining rows with blank lines
        for (int i = printed; i < logRows; ++i) {
            out << ERASE_LINE << "\n";
        }
    }

    // Help bar (cursor is already on the right line after the \n above)
    // [F] is shown ONLY when the fix is actionable right now:
    //   - initial diagnosis completed
    //   - IPFS is not already announcing a direct address
    //   - port 4001 is externally reachable (otherwise fix would make it worse)
    //   - the user hasn't already pressed F (in-flight or awaiting restart)
    // Once any of those stops being true, [F] disappears from the help bar.
    bool showFixKey = false;
    {
        std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
        showFixKey = _ipfsAnnounceChecked &&
                     !_ipfsAnnouncedDirectly &&
                     !_ipfsAnnounceFixApplied &&
                     _ipfsPort4001Open;
    }
    out << ERASE_LINE << DIM << " [Q] Quit  [A] Assets  [P] Ports  [L] Log Level";
    if (showFixKey) {
        out << "  " << RESET << FG_YELLOW << "[F] Fix IPFS" << RESET << DIM;
    }
    out << "  [V] Verify  [H] Info" << RESET;

    // Write everything in one shot to minimize flicker
    std::cout << out.str() << std::flush;
}

// ---- RPC server liveness probe ---------------------------------------------

/**
 * Background probe of the RPC server's liveness. Treats a non-null AppMain
 * RPC::Server pointer as "listening", reads the configured rpcassetport (default
 * 14024) for display, and publishes the result under _rpcProbeMutex for render().
 */
void ConsoleDashboard::probeRpcServer() {
    // Trust the pointer in AppMain. main.cpp pins the RPC::Server via a
    // shared_ptr for the whole program lifetime, and the accept loop catches
    // all exceptions internally, so the "dangling pointer" concern the
    // original win.26 TCP-connect probe was guarding against hasn't actually
    // happened in practice. The TCP probe itself was unreliable: on Windows
    // localhost, non-blocking connect() returns WSAEWOULDBLOCK and the
    // follow-up select() wait for writability sometimes times out at 200ms
    // even though the kernel 3-way handshake has already completed — which
    // made the dashboard show "Off" for a working RPC server.
    RPC::Server* server = AppMain::GetInstance()->getRpcServerIfSet();
    bool listening = (server != nullptr);

    // Read the configured port for display. Default matches RPC::Server::Server.
    unsigned int port = 14024;
    try {
        Config config("config.cfg");
        port = (unsigned int)config.getInteger("rpcassetport", 14024);
    } catch (...) {}

    std::lock_guard<std::mutex> lock(_rpcProbeMutex);
    _rpcListening = listening;
    _rpcProbedPort = port;
    _rpcProbed = true;
    _lastRpcProbe = std::chrono::steady_clock::now();
}

// ---- PSP registration status ------------------------------------------------

// ---- IPFS bitswap stats poll -----------------------------------------------
//
// Answers "is this node actually serving DigiAsset content out to the
// network?" by polling Kubo's stats/bitswap endpoint. BlocksSent is a
// monotonic counter incremented every time we hand a block to a remote peer.
// A non-zero rate proves outbound bitswap traffic is flowing.
//
// This only tests "serving something to someone"; it doesn't prove we're
// reachable by arbitrary external nodes or that DigiAsset content specifically
// is flowing. For that, see Part B (permanent-page coverage).
void ConsoleDashboard::pollBitswapStats() {
    // Can't call the file-local getIpfsApiBase() helper from here because it
    // lives in an anonymous namespace defined later in this file. Inline.
    std::string apiBase = "http://localhost:5001/api/v0/";
    try {
        Config config("config.cfg");
        std::string p = config.getString("ipfspath", "http://localhost:5001/api/v0/");
        if (!p.empty() && p.back() != '/') p += '/';
        apiBase = p;
    } catch (...) {}

    uint64_t blocksSent = 0;
    uint64_t dataSent = 0;
    bool available = false;
    try {
        std::string body = CurlHandler::post(apiBase + "stats/bitswap", {}, 5000);
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(body, root)) {
            if (root.isMember("BlocksSent")) blocksSent = root["BlocksSent"].asUInt64();
            if (root.isMember("DataSent")) dataSent = root["DataSent"].asUInt64();
            available = true;
        }
    } catch (...) {
        available = false;
    }

    auto now = std::chrono::steady_clock::now();

    double rateToReport = 0.0;
    {
        std::lock_guard<std::mutex> lock(_bitswapMutex);
        if (available) {
            // Compute rate: delta blocks / delta minutes. First probe seeds the
            // baseline and reports 0.0 rate (we don't have a baseline to diff from).
            if (_bitswapProbed) {
                double elapsedMin = std::chrono::duration<double>(now - _bitswapPrevTime).count() / 60.0;
                if (elapsedMin > 0.0 && blocksSent >= _bitswapBlocksSentPrev) {
                    _bitswapBlocksPerMin = (double)(blocksSent - _bitswapBlocksSentPrev) / elapsedMin;
                }
            }
            _bitswapBlocksSentPrev = blocksSent;
            _bitswapPrevTime = now;
            _bitswapBlocksSent = blocksSent;
            _bitswapDataSent = dataSent;
            rateToReport = _bitswapBlocksPerMin;
        }
        _bitswapAvailable = available;
        _bitswapProbed = true;
        _lastBitswapPoll = now;
    }

    // Mirror into NodeStats so the getnodestats RPC method returns fresh data
    // without having to make its own HTTP call to kubo.
    NodeStats::instance().setBitswap(available, blocksSent, dataSent, rateToReport);
}

// ---- Permanent-list coverage check -----------------------------------------
//
// Downloads mctrivia's /permanent/<page>.json pages (0..N, stopping when a
// page returns the error envelope), extracts every unique assetId, and
// queries the local Database to count how many we have. Reports coverage %.
// A correctly-synced node running a correct chain analyzer should have 100%.
//
// Any missing assetIds are logged at WARNING so the user sees them in the
// normal log stream — a <100% coverage is a real problem that indicates a
// chain-analyzer bug or a failed sync range.
void ConsoleDashboard::checkPermanentCoverage() {
    Log* log = Log::GetInstance();

    // Walk pages until we hit an error envelope. mctrivia's server returns
    // {"error":"..."} beyond the highest populated page.
    std::vector<std::string> trackedAssetIds;
    for (unsigned int page = 0; page < 100; page++) {
        std::string url = "https://ipfs.digiassetx.com/permanent/" +
                          std::to_string(page) + ".json";
        std::string body;
        try {
            body = CurlHandler::get(url, 10000);
        } catch (...) {
            break;
        }

        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(body, root)) break;
        if (root.isMember("error")) break;

        // Extract unique assetIds from the "changes" keys. Each key is
        // "<assetId>-<txhash>"; the assetId is everything before the first '-'.
        if (root.isMember("changes") && root["changes"].isObject()) {
            const Json::Value& changes = root["changes"];
            for (auto it = changes.begin(); it != changes.end(); ++it) {
                std::string key = it.key().asString();
                size_t dash = key.find('-');
                if (dash == std::string::npos) continue;
                trackedAssetIds.push_back(key.substr(0, dash));
            }
        }
    }

    // Dedupe
    std::sort(trackedAssetIds.begin(), trackedAssetIds.end());
    trackedAssetIds.erase(std::unique(trackedAssetIds.begin(), trackedAssetIds.end()),
                          trackedAssetIds.end());

    unsigned int tracked = (unsigned int)trackedAssetIds.size();
    unsigned int have = 0;
    std::vector<std::string> missing;

    // Query local db. Use getAssetIndex — returns assetIndex or throws if
    // unknown. Database must be initialized for this to work; if not, bail
    // out cleanly and retry next iteration.
    Database* db = AppMain::GetInstance()->getDatabaseIfSet();
    if (!db) {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        _coverageChecked = false; // keep "probing" state
        _lastCoverageCheck = std::chrono::steady_clock::now();
        return;
    }

    // Use getAssetIndexes() which returns the full vector of matching rows.
    // Non-empty = we have at least one row with this assetId. Empty or throw
    // = missing. getAssetIndex() singular throws if there are multiple
    // matches, which would give us false negatives on legitimate duplicates.
    for (const std::string& assetId: trackedAssetIds) {
        try {
            auto indexes = db->getAssetIndexes(assetId);
            if (!indexes.empty()) {
                have++;
            } else {
                missing.push_back(assetId);
            }
        } catch (...) {
            missing.push_back(assetId);
        }
    }

    // "Missing" here means "this permanent-list asset is not in our local chain
    // database YET" - i.e. we haven't decoded the block it was issued in. During
    // initial sync that is completely normal and NOT a problem (it resolves as we
    // catch up), so only shout about it once we are fully synced. (Note: this is
    // a CHAIN-DECODE progress measure, not IPFS pin status - the permanent CIDs
    // are pinned separately by the PSP fetcher thread. See PERMANENT-STORAGE.md.)
    ChainAnalyzer* covAnalyzer = AppMain::GetInstance()->getChainAnalyzerIfSet();
    bool fullySynced = covAnalyzer && (covAnalyzer->getSync() == ChainAnalyzer::SYNCED);
    if (!missing.empty() && !fullySynced) {
        // Still syncing - calm, single INFO line, no per-asset "missing" spam.
        log->addMessage("Permanent-list: " + std::to_string(have) + "/" +
                                std::to_string(tracked) +
                                " assets decoded so far (still syncing - the rest appear as you catch up)",
                        Log::INFO);
    } else if (!missing.empty()) {
        // Fully synced but still missing entries: a genuine gap worth a WARNING.
        log->addMessage("Permanent-list coverage: " + std::to_string(have) +
                                "/" + std::to_string(tracked) +
                                " (" + std::to_string(missing.size()) + " missing)",
                        Log::WARNING);
        // Cap the per-line noise: log up to 5 missing assetIds, then a summary.
        size_t toShow = std::min<size_t>(5, missing.size());
        for (size_t i = 0; i < toShow; i++) {
            log->addMessage("  missing asset: " + missing[i], Log::WARNING);
        }
        if (missing.size() > toShow) {
            log->addMessage("  ... and " + std::to_string(missing.size() - toShow) +
                                    " more (enable DEBUG for the full list)",
                            Log::WARNING);
        }
        for (size_t i = toShow; i < missing.size(); i++) {
            log->addMessage("  missing asset: " + missing[i], Log::DEBUG);
        }
    } else if (tracked > 0) {
        // Steady-state "all good" line fires every ~10 min; DEBUG so it doesn't
        // spam INFO. The missing-coverage branch above stays WARNING.
        log->addMessage("Permanent-list coverage: " + std::to_string(have) +
                                "/" + std::to_string(tracked) + " (100%)",
                        Log::DEBUG);
    }

    {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        _coverageTrackedCount = tracked;
        _coverageHaveCount = have;
        _coverageChecked = true;
        _lastCoverageCheck = std::chrono::steady_clock::now();
    }

    // Mirror into NodeStats so the getnodestats RPC method can report coverage
    // without having to re-walk all permanent pages.
    NodeStats::instance().setCoverage(tracked, have);
}

/**
 * Returns the base URL of the pool this node actually joined (its subscribed
 * networked pool - psp2 DigiStamp on new nodes, psp1 mctrivia on legacy),
 * falling back to https://pool.digistamp.co and stripping trailing slashes.
 * Shared by the PSP registration check and the pool-node listing.
 */
std::string ConsoleDashboard::getConfiguredPoolBase() {
    std::string base;
    auto* pspList = AppMain::GetInstance()->getPermanentStoragePoolListIfSet();
    auto* pool = pspList ? pspList->getActiveNetworkedPool() : nullptr;
    if (pool) {
        base = pool->getURL();
    } else {
        // List not built yet, or no pool joined - fall back to the DigiStamp key,
        // then the legacy key, then the public default.
        try {
            Config config("config.cfg");
            base = config.getString("psp2server", config.getString("psp1server", "https://pool.digistamp.co"));
        } catch (...) {
            base = "https://pool.digistamp.co";
        }
    }
    if (base.empty()) base = "https://pool.digistamp.co";
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

/**
 * Background check of the configured pool's health. Fetches the pool's
 * /nodes.json, counts how many nodes it reports online (by counting "id" keys),
 * and publishes that count plus a "Pool reachable"/"Pool unreachable" status
 * under _pspStatusMutex. The HTTP fetch is done outside the lock; a success is
 * timestamped to cache for ~10 min while a failure is left uncached to retry.
 */
void ConsoleDashboard::checkPspRegistration() {
    auto now = std::chrono::steady_clock::now();

    // Count the nodes the CONFIGURED pool (psp1server) reports online. We use
    // /nodes.json (array of {id: peerId}) so the count reflects the pool this
    // node actually uses — not a hardcoded server. Press [N] to list them and
    // self-check whether our node is included.
    //
    // Do the HTTP fetch OUTSIDE the lock so we don't block render() for 5s on a
    // slow network. Only grab the lock when committing the result.
    try {
        std::string url = getConfiguredPoolBase() + "/nodes.json";
        std::string resp = CurlHandler::get(url, 5000);
        int nodeCount = 0;
        size_t pos = 0;
        while ((pos = resp.find("\"id\"", pos)) != std::string::npos) {
            nodeCount++;
            pos += 4;
        }
        {
            std::lock_guard<std::mutex> lock(_pspStatusMutex);
            _pspNodeCount = nodeCount;
            _pspStatus = "Pool reachable";
            _lastPspCheck = now; // cache for 10 min
        }
        // Mirror to NodeStats so the web console can show pool reachability + node
        // count without doing its own /nodes.json fetch on the request thread.
        NodeStats::instance().setPool(true, (unsigned int)nodeCount, getConfiguredPoolBase());
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(_pspStatusMutex);
            _pspStatus = "Pool unreachable";
            // don't cache failure — retry next refresh
        }
        NodeStats::instance().setPool(false, 0, getConfiguredPoolBase());
    }
}

/**
 * [N] command handler. Fetches the configured pool's /nodes.json, parses each
 * node's id, and logs a capped list showing each node's IP and a shortened
 * peerId. Scans the full list (not just the shown ones) to self-check whether
 * this node's own IPFS peerId appears, reporting OK / not-listed / unknown-id.
 * Output goes to the log pane. Runs on a detached thread.
 */
void ConsoleDashboard::listPoolNodes() {
    Log* log = Log::GetInstance();

    // Normalize any peerId or multiaddr to the bare peerId (the final
    // /p2p/<id> segment), so we can compare our own id against the pool's list
    // regardless of which form each side stored.
    auto barePeer = [](std::string s) -> std::string {
        size_t p = s.rfind("/p2p/");
        if (p != std::string::npos) s = s.substr(p + 5);
        size_t slash = s.find('/');
        if (slash != std::string::npos) s = s.substr(0, slash);
        return s;
    };

    // Pull the IPv4/IPv6 address out of a multiaddr like
    // /ip4/1.2.3.4/tcp/4001/p2p/<id> for display. Empty if none present.
    auto extractIp = [](const std::string& s) -> std::string {
        size_t p = s.find("/ip4/");
        if (p == std::string::npos) p = s.find("/ip6/");
        if (p == std::string::npos) return "";
        p += 5;
        size_t e = s.find('/', p);
        return s.substr(p, (e == std::string::npos ? s.size() : e) - p);
    };

    // Our own peerId (for the self-check).
    std::string myId;
    try {
        IPFS* ipfs = AppMain::GetInstance()->getIPFSIfSet();
        if (ipfs) myId = barePeer(ipfs->getPeerId());
    } catch (...) {}

    std::string base = getConfiguredPoolBase();
    std::string url = base + "/nodes.json";
    log->addMessage("--- Pool Nodes (" + url + ") ---");

    std::string resp;
    try {
        resp = CurlHandler::get(url, 8000);
    } catch (...) {
        log->addMessage("  Could not reach the pool's /nodes.json. Is psp2server correct and the pool up?");
        return;
    }

    // Parse each "id":"<value>" entry.
    std::vector<std::string> ids;
    size_t pos = 0;
    while ((pos = resp.find("\"id\"", pos)) != std::string::npos) {
        size_t colon = resp.find(':', pos + 4);
        if (colon == std::string::npos) break;
        size_t q1 = resp.find('"', colon + 1);
        if (q1 == std::string::npos) break;
        size_t q2 = resp.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        ids.push_back(resp.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
    }

    if (ids.empty()) {
        log->addMessage("  The pool reports 0 nodes online.");
    } else {
        log->addMessage("  " + std::to_string(ids.size()) + " node(s) online:");
    }

    // Cap the console dump so a large pool doesn't flood the log area; the
    // self-check below still scans ALL nodes. The full list is always on the
    // pool's /nodes.json.
    const size_t MAX_SHOW = 30;
    bool foundSelf = false;
    for (size_t i = 0; i < ids.size(); i++) {
        std::string bare = barePeer(ids[i]);
        bool isSelf = (!myId.empty() && bare == myId);
        if (isSelf) foundSelf = true;
        if (i >= MAX_SHOW) continue;

        // Show IP (left, padded) + a shortened peerId so nodes are easy to tell
        // apart. The full peerId of our own node is printed in the summary line.
        std::string ip = extractIp(ids[i]);
        std::string ipCol = ip.empty() ? "(unknown IP)" : ip;
        if (ipCol.size() < 16) ipCol += std::string(16 - ipCol.size(), ' ');
        std::string shortId = bare.size() > 21
                                  ? bare.substr(0, 12) + ".." + bare.substr(bare.size() - 6)
                                  : bare;
        log->addMessage("   " + ipCol + " " + shortId + (isSelf ? "   <-- YOU" : ""));
    }
    if (ids.size() > MAX_SHOW) {
        log->addMessage("   ...and " + std::to_string(ids.size() - MAX_SHOW) +
                        " more. Full list: " + base + "/nodes.json");
    }

    // Self-check summary.
    if (myId.empty()) {
        log->addMessage("  Self-check: could not read your own peerId (is IPFS running?).");
    } else if (foundSelf) {
        log->addMessage("  Self-check: OK - your node IS registered and visible in this pool.");
    } else {
        log->addMessage("  Self-check: your node is NOT listed. Your peerId: " + myId);
        log->addMessage("    Registration can take a few minutes; forwarding IPFS port 4001 helps.");
    }
}

// ---- Payout balance ---------------------------------------------------------

/**
 * Loads the payout address (config key psp1payout, falling back to psp0payout)
 * once, then refreshes the address balance at most every 4 hours by querying the
 * cryptoid.info DGB balance API and formatting it to 4 decimals. On failure the
 * balance shows "unavailable". Called from render() every frame but rate-limited
 * internally.
 */
void ConsoleDashboard::loadPayoutInfo() {
    if (!_payoutLoaded) {
        try {
            Config config("config.cfg");
            // Use whichever networked pool this node actually joined (psp2 on new
            // nodes, psp1 on legacy) instead of a hardcoded index; then fall back
            // through psp2 / psp1 / psp0 payout keys.
            std::string payoutKey;
            auto* pspList = AppMain::GetInstance()->getPermanentStoragePoolListIfSet();
            auto* pool = pspList ? pspList->getActiveNetworkedPool() : nullptr;
            if (pool) payoutKey = "psp" + std::to_string(pool->getPoolIndex()) + "payout";
            _payoutAddress = payoutKey.empty() ? std::string("") : config.getString(payoutKey, "");
            if (_payoutAddress.empty()) _payoutAddress = config.getString("psp2payout", "");
            if (_payoutAddress.empty()) _payoutAddress = config.getString("psp1payout", "");
            if (_payoutAddress.empty()) _payoutAddress = config.getString("psp0payout", "");
        } catch (...) {}
        _payoutLoaded = true;
        _lastBalanceTime = std::chrono::steady_clock::now() - std::chrono::seconds(120); // force immediate fetch
    }

    if (_payoutAddress.empty()) return;

    // Mirror to NodeStats every frame so the web console shows the payout address
    // immediately and picks up the balance one frame after the periodic fetch below.
    NodeStats::instance().setPayout(_payoutAddress, _payoutBalance);

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - _lastBalanceTime).count();
    if (elapsed < 14400.0 && !_payoutBalance.empty()) return; // refresh every 4 hours

    try {
        std::string response = CurlHandler::get(
            "http://chainz.cryptoid.info/dgb/api.dws?q=getbalance&a=" + _payoutAddress, 5000);
        // Response is a balance string like "21.2103" (varying decimal precision)
        // Trim whitespace
        while (!response.empty() && (response.back() == '\n' || response.back() == '\r' || response.back() == ' ')) {
            response.pop_back();
        }
        // Format to exactly 4 decimal places
        try {
            double balance = std::stod(response);
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(4) << balance;
            _payoutBalance = oss.str() + " DGB";
        } catch (...) {
            _payoutBalance = response + " DGB"; // fallback to raw response
        }
        _lastBalanceTime = now;
    } catch (...) {
        if (_payoutBalance.empty()) _payoutBalance = "unavailable";
    }
}

// ---- IPFS announce detection & repair --------------------------------------

namespace {
    // Percent-encodes a string for safe use in a URL query component (leaves the
    // RFC 3986 unreserved set A-Z a-z 0-9 - _ . ~ untouched).
    std::string urlEncodeComponent(const std::string& s) {
        std::string out;
        out.reserve(s.size() * 3);
        for (unsigned char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                out += (char)c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                out += buf;
            }
        }
        return out;
    }

    // Read ipfspath from config.cfg, default to Kubo's standard HTTP API.
    std::string getIpfsApiBase() {
        try {
            Config config("config.cfg");
            std::string p = config.getString("ipfspath", "http://localhost:5001/api/v0/");
            if (!p.empty() && p.back() != '/') p += '/';
            return p;
        } catch (...) {
            return "http://localhost:5001/api/v0/";
        }
    }
}

/**
 * Background diagnosis of IPFS external reachability. Reads the WAN IP from the
 * web server; checks whether Kubo's /id already announces an address containing
 * that IP; if not, probes whether TCP 4001 is externally reachable. Publishes
 * the findings and a user-facing hint under _ipfsAnnounceMutex — telling the user
 * to press [F] when the port is open but not announced, that a relay path is
 * fine when the port is closed, or to restart IPFS after a fix was applied. Bails
 * quietly if the WAN IP is unknown or the IPFS API is unreachable.
 */
void ConsoleDashboard::checkIpfsAnnounce() {
    // Snapshot the WAN IP from the web server (already resolved via ipify at startup)
    AppMain* app = AppMain::GetInstance();
    WebServer* ws = app->getWebServerIfSet();
    std::string wanIp;
    if (ws) wanIp = ws->getExternalIP();
    if (wanIp.empty() || wanIp == "unknown") {
        std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
        _ipfsAnnounceHint.clear();
        return;
    }

    std::string apiBase = getIpfsApiBase();

    // Step 1: does IPFS /id already list an address containing our WAN IP?
    bool announced = false;
    try {
        std::string idJson = CurlHandler::post(apiBase + "id", {}, 5000);
        // Cheap substring check — we don't need full JSON parsing for this.
        if (idJson.find(wanIp) != std::string::npos) {
            announced = true;
        }
    } catch (...) {
        // IPFS API unreachable — can't diagnose, bail out quietly
        std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
        _ipfsAnnounceHint.clear();
        _ipfsAnnounceChecked = false;
        return;
    }

    // Step 2: if not announced, probe port 4001 externally to see whether
    // fixing is even possible. If the port isn't reachable, setting
    // Addresses.Announce would advertise a dead address.
    bool portOpen = false;
    if (!announced) {
        try {
            std::string resp = CurlHandler::get("http://ifconfig.co/port/4001", 10000);
            if (resp.find("\"reachable\": true") != std::string::npos ||
                resp.find("\"reachable\":true") != std::string::npos) {
                portOpen = true;
            }
        } catch (...) {
            // leave portOpen = false
        }
    }

    std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
    _ipfsAnnouncedDirectly = announced;
    _ipfsPort4001Open = portOpen;
    _ipfsAnnounceChecked = true;
    _lastIpfsAnnounceCheck = std::chrono::steady_clock::now();

    if (announced) {
        // Convergence reached: the fix (manual or auto) has taken effect
        _ipfsAnnounceFixApplied = false;
        _ipfsAnnounceHint.clear();
    } else if (_ipfsAnnounceFixApplied) {
        // Fix was applied but Kubo hasn't picked it up yet — keep reminding
        _ipfsAnnounceHint = "Waiting for IPFS Desktop restart to pick up new announce list...";
    } else if (portOpen) {
        _ipfsAnnounceHint = "Port 4001 is open but IPFS isn't announcing it - press [F] to fix";
    } else {
        _ipfsAnnounceHint.clear(); // NAT'd with no port forward — relay path is the right answer
    }
}

/**
 * [F] command handler. Sets Kubo's Addresses.Announce to advertise our WAN IP on
 * TCP and QUIC 4001 via the IPFS config HTTP API, so a port-forwarded but not-yet
 * -announced node becomes directly dialable. Guards against misuse: aborts if the
 * WAN IP is unknown, diagnosis hasn't run, IPFS already announces directly, or
 * port 4001 isn't reachable. On success it flags _ipfsAnnounceFixApplied so
 * render() polls aggressively until Kubo (after a manual IPFS Desktop restart)
 * picks up the change. Returns true if the config was set. Runs on a detached
 * thread.
 */
bool ConsoleDashboard::applyIpfsAnnounceFix() {
    Log* log = Log::GetInstance();
    AppMain* app = AppMain::GetInstance();

    // Must have a known WAN IP to announce
    WebServer* ws = app->getWebServerIfSet();
    std::string wanIp;
    if (ws) wanIp = ws->getExternalIP();
    if (wanIp.empty() || wanIp == "unknown") {
        log->addMessage("Fix aborted: external IP not known yet", Log::WARNING);
        return false;
    }

    // Sanity check: only run if our diagnosis said the port is open. This
    // prevents setting Announce to a dead address on a NAT'd box.
    {
        std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
        if (!_ipfsAnnounceChecked) {
            log->addMessage("Fix aborted: diagnosis hasn't run yet, please wait a moment", Log::WARNING);
            return false;
        }
        if (_ipfsAnnouncedDirectly) {
            log->addMessage("Nothing to fix: IPFS is already announcing a direct address");
            return false;
        }
        if (!_ipfsPort4001Open) {
            log->addMessage("Fix aborted: port 4001 is NOT reachable from the internet. "
                            "Forward TCP 4001 on your router first (press [P] to recheck).",
                            Log::WARNING);
            return false;
        }
    }

    // Build the JSON array argument: ["/ip4/<wan>/tcp/4001","/ip4/<wan>/udp/4001/quic-v1"]
    std::string jsonArray = "[\"/ip4/" + wanIp + "/tcp/4001\",\"/ip4/" + wanIp + "/udp/4001/quic-v1\"]";

    // Build the IPFS config URL: POST .../config?arg=Addresses.Announce&arg=<encoded>&json=true
    std::string apiBase = getIpfsApiBase();
    std::string url = apiBase + "config?arg=Addresses.Announce&arg="
                      + urlEncodeComponent(jsonArray) + "&json=true";

    log->addMessage("Setting Kubo Addresses.Announce = " + jsonArray);
    try {
        std::string response = CurlHandler::post(url, {}, 10000);
        // Kubo echoes the new config on success; look for our WAN IP in the response.
        if (response.find(wanIp) == std::string::npos) {
            log->addMessage("IPFS config set returned unexpected response: " + response.substr(0, 200),
                            Log::WARNING);
            return false;
        }
    } catch (const std::exception& e) {
        log->addMessage(std::string("IPFS config set failed: ") + e.what(), Log::WARNING);
        return false;
    }

    log->addMessage("Addresses.Announce updated successfully.");
    log->addMessage("IMPORTANT: restart IPFS Desktop (tray icon -> Quit, relaunch) "
                    "to activate the new announce list.", Log::WARNING);
    log->addMessage("After IPFS restart, DigiAssetWindows will verify the change "
                    "automatically (checking every 15s).");

    // Mark the fix as applied so the render loop polls aggressively until
    // Kubo picks up the new announce list. Reset _lastIpfsAnnounceCheck
    // to zero so the next render triggers an immediate re-check.
    {
        std::lock_guard<std::mutex> lock(_ipfsAnnounceMutex);
        _ipfsAnnounceFixApplied = true;
        _ipfsAnnounceHint = "Addresses.Announce set - restart IPFS Desktop to activate";
        _lastIpfsAnnounceCheck = std::chrono::steady_clock::time_point{};
    }
    return true;
}

// ---- [V] Verify: dump swarm-connect commands for announced multiaddrs ------

/**
 * [V] command handler. Fetches Kubo's /id, extracts our peerId and announced
 * multiaddrs, filters out LAN/loopback/link-local addresses, and logs a
 * ready-to-paste `ipfs swarm connect <addr>/p2p/<peerId>` line for each public
 * address plus a follow-up verify command. Lets the user confirm from a remote
 * box that our announced addresses are actually dialable. Runs on a detached
 * thread; logs a warning if the API is unreachable or no public addresses exist.
 */
void ConsoleDashboard::printSwarmConnectCommands() {
    Log* log = Log::GetInstance();

    std::string idJson;
    try {
        idJson = CurlHandler::post(getIpfsApiBase() + "id", {}, 5000);
    } catch (const std::exception& e) {
        log->addMessage(std::string("Verify failed: IPFS API unreachable: ") + e.what(),
                        Log::WARNING);
        return;
    }

    // Extract "ID":"<peerId>"
    std::string peerId;
    {
        size_t k = idJson.find("\"ID\"");
        if (k != std::string::npos) {
            size_t colon = idJson.find(':', k);
            size_t q1 = (colon != std::string::npos) ? idJson.find('"', colon + 1) : std::string::npos;
            size_t q2 = (q1 != std::string::npos) ? idJson.find('"', q1 + 1) : std::string::npos;
            if (q2 != std::string::npos) peerId = idJson.substr(q1 + 1, q2 - q1 - 1);
        }
    }
    if (peerId.empty()) {
        log->addMessage("Verify failed: could not parse peer ID from /id response", Log::WARNING);
        return;
    }

    // Extract "Addresses":[ ... ]
    std::vector<std::string> addrs;
    {
        size_t k = idJson.find("\"Addresses\"");
        size_t start = (k != std::string::npos) ? idJson.find('[', k) : std::string::npos;
        size_t end = (start != std::string::npos) ? idJson.find(']', start) : std::string::npos;
        if (start != std::string::npos && end != std::string::npos) {
            size_t pos = start + 1;
            while (pos < end) {
                size_t q1 = idJson.find('"', pos);
                if (q1 == std::string::npos || q1 >= end) break;
                size_t q2 = idJson.find('"', q1 + 1);
                if (q2 == std::string::npos || q2 >= end) break;
                addrs.push_back(idJson.substr(q1 + 1, q2 - q1 - 1));
                pos = q2 + 1;
            }
        }
    }
    if (addrs.empty()) {
        log->addMessage("Verify failed: no addresses in /id response", Log::WARNING);
        return;
    }

    // Filter out LAN/loopback/link-local — can't be dialed from another host.
    auto isLan = [](const std::string& a) -> bool {
        size_t p = a.find("/ip4/");
        if (p != std::string::npos) {
            p += 5;
            size_t e = a.find('/', p);
            std::string ip = (e == std::string::npos) ? a.substr(p) : a.substr(p, e - p);
            if (ip.rfind("127.", 0) == 0) return true;
            if (ip.rfind("10.", 0) == 0) return true;
            if (ip.rfind("192.168.", 0) == 0) return true;
            if (ip.rfind("169.254.", 0) == 0) return true;
            if (ip.rfind("172.", 0) == 0) {
                try {
                    int o2 = std::stoi(ip.substr(4));
                    if (o2 >= 16 && o2 <= 31) return true;
                } catch (...) {}
            }
            return false;
        }
        if (a.find("/ip6/::1") != std::string::npos) return true;
        if (a.find("/ip6/fe80") != std::string::npos) return true;
        return false;
    };

    log->addMessage("--- Verify: run these from a known-good remote box (e.g. your Linux node) ---");
    log->addMessage("Peer ID: " + peerId);
    std::string selfSuffix = "/p2p/" + peerId;

    int emitted = 0;
    for (const auto& a : addrs) {
        if (isLan(a)) continue;

        // Kubo sometimes omits the trailing /p2p/<selfId>; swarm connect needs it.
        std::string full = a;
        if (full.size() < selfSuffix.size() ||
            full.compare(full.size() - selfSuffix.size(), selfSuffix.size(), selfSuffix) != 0) {
            full += selfSuffix;
        }
        log->addMessage("  ipfs swarm connect " + full);
        emitted++;
    }

    if (emitted == 0) {
        log->addMessage("  (no public addresses announced - press [F] to set Announce, "
                        "or wait for relay addresses to appear)", Log::WARNING);
    } else {
        log->addMessage("Then: ipfs swarm peers | grep " + peerId.substr(0, 20));
        log->addMessage("If the connect succeeds, the pool can reach you too.");
    }
    log->addMessage("--- end verify ---");
}

// ---- Port checking ----------------------------------------------------------

/**
 * [P] command handler. For each relevant inbound port (IPFS swarm 4001, the web
 * UI port, DigiByte P2P 12024) asks ifconfig.co/port/<n> to attempt an external
 * TCP connect back to our WAN IP, and logs each as Open/Closed (WARNING when not
 * reachable). Aborts if the external IP is unknown. Runs on a detached thread.
 */
void ConsoleDashboard::checkPorts() {
    Log* log = Log::GetInstance();
    AppMain* app = AppMain::GetInstance();

    // Get external IP
    WebServer* ws = app->getWebServerIfSet();
    std::string externalIP;
    if (ws) {
        externalIP = ws->getExternalIP();
    }
    if (externalIP.empty() || externalIP == "unknown") {
        log->addMessage("Cannot check ports: external IP unknown", Log::WARNING);
        return;
    }

    // Collect ports to check
    struct PortInfo {
        int port;
        std::string name;
    };
    std::vector<PortInfo> ports;
    ports.push_back({4001, "IPFS Swarm"});
    if (ws) {
        ports.push_back({(int)ws->getPort(), "Web UI"});
    }
    ports.push_back({12024, "DigiByte P2P"});

    log->addMessage("--- Port Check (external IP: " + externalIP + ") ---");

    for (const auto& p : ports) {
        std::string status;
        try {
            // ifconfig.co/port/N does an external TCP connect to our IP on port N
            // Returns JSON: {"ip":"...","port":N,"reachable":true/false}
            std::string response = CurlHandler::get(
                "http://ifconfig.co/port/" + std::to_string(p.port), 10000);
            if (response.find("\"reachable\": true") != std::string::npos ||
                response.find("\"reachable\":true") != std::string::npos) {
                status = "Open";
            } else {
                status = "Closed";
            }
        } catch (...) {
            status = "Check failed";
        }

        if (status == "Open") {
            log->addMessage("  Port " + std::to_string(p.port) + " (" + p.name + "): " + status);
        } else {
            log->addMessage("  Port " + std::to_string(p.port) + " (" + p.name + "): " + status, Log::WARNING);
        }
    }
}

// ---- Static helpers ---------------------------------------------------------

std::string ConsoleDashboard::centerText(const std::string& text, int width) {
    if ((int)text.size() >= width) return text;
    int pad = (width - (int)text.size()) / 2;
    return std::string(pad, ' ') + text;
}

std::string ConsoleDashboard::padRight(const std::string& text, int width) {
    if ((int)text.size() >= width) return text;
    return text + std::string(width - (int)text.size(), ' ');
}

/**
 * Formats a duration in seconds into a compact human string, auto-scaling to
 * sec / min / hours / days. Negative input returns "--". Used for ETA and uptime.
 */
std::string ConsoleDashboard::formatDuration(double seconds) {
    if (seconds < 0) return "--";
    if (seconds < 60) {
        return std::to_string((int)seconds) + " sec";
    } else if (seconds < 3600) {
        int mins = (int)(seconds / 60);
        return std::to_string(mins) + " min";
    } else if (seconds < 86400) {
        double hrs = seconds / 3600.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << hrs << " hours";
        return oss.str();
    } else {
        double days = seconds / 86400.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << days << " days";
        return oss.str();
    }
}

/** Formats an integer with thousands separators (e.g. 1234567 -> "1,234,567"). */
std::string ConsoleDashboard::formatNumber(uint64_t n) {
    std::string s = std::to_string(n);
    // Insert commas
    int pos = (int)s.size() - 3;
    while (pos > 0) {
        s.insert(pos, ",");
        pos -= 3;
    }
    return s;
}

/**
 * Renders a colored `[####    ]` progress bar of the given width, filled in
 * proportion to fraction (clamped to 0..1). Includes the VT100 green/reset codes.
 */
std::string ConsoleDashboard::progressBar(double fraction, int width) {
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;

    int filled = (int)(fraction * width);
    int empty = width - filled;

    std::string bar = FG_GREEN;
    bar += std::string("[");
    bar += std::string(filled, '#');
    bar += std::string(empty, ' ');
    bar += std::string("]");
    bar += RESET;
    return bar;
}
