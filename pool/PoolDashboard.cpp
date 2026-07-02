#include "PoolDashboard.h"
#include "CurlHandler.h"
#include "PoolDatabase.h"
#include "PoolServer.h"
#include "PoolVerifier.h"
#include "Version.h"
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#endif

// --- VT100 escape helpers --- (same subset ConsoleDashboard uses in main exe)
#define ESC "\033["
#define CURSOR_HOME    ESC "H"
#define ERASE_SCREEN   ESC "2J"
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
#define FG_BRIGHT_WHITE ESC "97m"

namespace {
    // Same format as ConsoleDashboard::formatDuration in the main exe.
    std::string formatDuration(int64_t seconds) {
        if (seconds < 0) return "--";
        if (seconds < 60) return std::to_string((int) seconds) + " sec";
        if (seconds < 3600) return std::to_string((int) (seconds / 60)) + " min";
        if (seconds < 86400) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << seconds / 3600.0 << " hours";
            return oss.str();
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << seconds / 86400.0 << " days";
        return oss.str();
    }

    std::string formatNumber(uint64_t n) {
        std::string s = std::to_string(n);
        std::string out;
        int count = 0;
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            if (count > 0 && count % 3 == 0) out += ',';
            out += *it;
            count++;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
}

PoolDashboard::PoolDashboard(PoolDatabase& db, PoolServer& server, PoolVerifier& verifier,
                             const std::string& configPath)
    : _db(db), _server(server), _verifier(verifier), _configPath(configPath),
      _startTime(std::chrono::system_clock::now()) {
}

namespace {
    // Re-read pool.cfg on demand (same tiny parser as main.cpp).
    std::map<std::string, std::string> readPoolConfig(const std::string& path) {
        std::map<std::string, std::string> out;
        std::ifstream in(path);
        if (!in) return out;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            out[line.substr(0, eq)] = line.substr(eq + 1);
        }
        return out;
    }

    std::string sendToAddress(const std::string& rpcUser, const std::string& rpcPass,
                              int rpcPort, const std::string& address, double amountDgb) {
        // Build the JSON-RPC request for sendtoaddress.
        char amountBuf[64];
        snprintf(amountBuf, sizeof(amountBuf), "%.8f", amountDgb);

        std::string body = "{\"jsonrpc\":\"1.0\",\"id\":\"pool\",\"method\":\"sendtoaddress\","
                           "\"params\":[\"" + address + "\"," + amountBuf + "]}";
        std::string url = "http://" + rpcUser + ":" + rpcPass + "@127.0.0.1:" +
                          std::to_string(rpcPort);

        std::string respBody;
        long status = 0;
        try {
            status = CurlHandler::postJson(url, body, respBody, 30000);
        } catch (const std::exception& e) {
            return std::string("ERROR: ") + e.what();
        }

        if (status < 200 || status >= 300) {
            return "ERROR: HTTP " + std::to_string(status) + " " + respBody;
        }

        // DigiByte returns {"result":"<txid>","error":null,...} on success and
        // {"result":null,"error":{...}} on failure. Check "error" FIRST — if it's
        // a non-null object the send failed, and we must never mistake the error
        // body for a txid (which would record a failed payout as SENT).
        size_t epos = respBody.find("\"error\"");
        if (epos != std::string::npos) {
            size_t ec = respBody.find(':', epos + 7);
            size_t ev = (ec != std::string::npos) ? respBody.find_first_not_of(" \t", ec + 1) : std::string::npos;
            if (ev != std::string::npos && respBody.compare(ev, 4, "null") != 0) {
                return "ERROR: " + respBody;
            }
        }
        // Parse the result, which must be a JSON string (the txid).
        size_t rpos = respBody.find("\"result\"");
        if (rpos == std::string::npos) return "ERROR: no result field in response";
        size_t rc = respBody.find(':', rpos + 8);
        size_t rv = (rc != std::string::npos) ? respBody.find_first_not_of(" \t", rc + 1) : std::string::npos;
        if (rv == std::string::npos || respBody[rv] != '"') {
            // result is null or not a string — treat as failure.
            return "ERROR: " + respBody;
        }
        size_t q2 = respBody.find('"', rv + 1);
        if (q2 == std::string::npos) return "ERROR: " + respBody;
        return respBody.substr(rv + 1, q2 - rv - 1); // the txid
    }

    // Wallet spendable balance via DigiByte Core RPC. Negative sentinel on any
    // failure so callers can tell "0 DGB" from "couldn't reach core".
    double getWalletBalance(const std::string& rpcUser, const std::string& rpcPass, int rpcPort) {
        if (rpcUser.empty()) return -1.0;
        std::string body = "{\"jsonrpc\":\"1.0\",\"id\":\"pool\",\"method\":\"getbalance\",\"params\":[]}";
        std::string url = "http://" + rpcUser + ":" + rpcPass + "@127.0.0.1:" + std::to_string(rpcPort);
        std::string resp;
        long status = 0;
        try { status = CurlHandler::postJson(url, body, resp, 15000); }
        catch (...) { return -1.0; }
        if (status < 200 || status >= 300) return -1.0;
        size_t rpos = resp.find("\"result\"");
        if (rpos == std::string::npos) return -1.0;
        size_t colon = resp.find(':', rpos + 8);
        if (colon == std::string::npos) return -1.0;
        size_t start = resp.find_first_not_of(" \t", colon + 1);
        if (start == std::string::npos) return -1.0;
        try { return std::stod(resp.substr(start)); } catch (...) { return -1.0; }
    }

    // Read a cfg key (map is const) with a default.
    std::string cfgGet(const std::map<std::string, std::string>& cfg,
                       const std::string& key, const std::string& def = "") {
        auto it = cfg.find(key);
        return it == cfg.end() ? def : it->second;
    }

    // Decide this period's total payout budget (DGB). If poolpayoutpercent is
    // set (>0, interpreted as a percent, e.g. 10 = 10%) the budget is that share
    // of the wallet's spendable balance — balance-derived, so it can never
    // overspend an empty wallet and auto-scales with donations. Otherwise fall
    // back to the fixed poolspendperperiod. `mode` is set for display; returns a
    // negative sentinel if a balance-derived budget was requested but the RPC
    // balance couldn't be read.
    double computeBudget(const std::map<std::string, std::string>& cfg, std::string& mode) {
        double percent = 0.0;
        try { percent = std::stod(cfgGet(cfg, "poolpayoutpercent", "0")); } catch (...) { percent = 0.0; }
        if (percent > 0.0) {
            if (percent > 100.0) percent = 100.0;
            std::string rpcUser = cfgGet(cfg, "rpcuser");
            std::string rpcPass = cfgGet(cfg, "rpcpassword");
            int rpcPort = 14022;
            try { rpcPort = std::stoi(cfgGet(cfg, "rpcport", "14022")); } catch (...) {}
            double bal = getWalletBalance(rpcUser, rpcPass, rpcPort);
            if (bal < 0.0) { mode = "balance unavailable (RPC error)"; return -1.0; }
            char m[96];
            snprintf(m, sizeof(m), "%.1f%% of %.4f DGB available", percent, bal);
            mode = m;
            return bal * (percent / 100.0);
        }
        double spend = 0.0;
        try { spend = std::stod(cfgGet(cfg, "poolspendperperiod", "0")); } catch (...) { spend = 0.0; }
        mode = "poolspendperperiod (fixed)";
        return spend;
    }
}

PoolDashboard::~PoolDashboard() {
    stop();
}

bool PoolDashboard::enableVT100() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return false;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return SetConsoleMode(hOut, mode) != 0;
#else
    return true;
#endif
}

void PoolDashboard::start() {
    if (_running.exchange(true)) return;
    std::cout << ESC "2J" << CURSOR_HOME << HIDE_CURSOR << std::flush;
    render();
    _thread = std::thread([this]() { this->refreshLoop(); });
}

void PoolDashboard::stop() {
    if (!_running.exchange(false)) return;
    if (_thread.joinable()) _thread.join();
    std::cout << SHOW_CURSOR << std::flush;
}

void PoolDashboard::addLog(const std::string& line) {
    std::lock_guard<std::mutex> lk(_logMutex);
    _logLines.push_back(line);
    while (_logLines.size() > MAX_LOG_LINES) _logLines.pop_front();
}

void PoolDashboard::refreshLoop() {
    while (_running.load()) {
        processInput();
        render();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void PoolDashboard::processInput() {
#ifdef _WIN32
    while (_kbhit()) {
        int ch = _getch();
        if (ch == 'q' || ch == 'Q' || ch == 3 /* Ctrl+C */) {
            _quit.store(true);
        } else if (_awaitingPayoutConfirm.load()) {
            // We're waiting for Y/N after an [E] press.
            _awaitingPayoutConfirm.store(false);
            if (ch == 'y' || ch == 'Y') {
                addLog("Executing payout...");
                auto cfg = readPoolConfig(_configPath);
                std::string rpcUser = cfg.count("rpcuser") ? cfg["rpcuser"] : "";
                std::string rpcPass = cfg.count("rpcpassword") ? cfg["rpcpassword"] : "";
                int rpcPort = 14022;
                try { if (cfg.count("rpcport")) rpcPort = std::stoi(cfg["rpcport"]); } catch (...) {}

                auto targets = _db.getVerifiedPayoutTargets();
                // Use the per-node amount computed and shown at [E] time so the
                // send matches exactly what the operator confirmed.
                double perNode = _pendingPerNode;
                int64_t perNodeSat = (int64_t)(perNode * 100000000.0);

                int success = 0;
                int failed = 0;
                for (const auto& t: targets) {
                    std::string result = sendToAddress(rpcUser, rpcPass, rpcPort,
                                                       t.payoutAddress, perNode);
                    if (result.substr(0, 5) == "ERROR") {
                        addLog("  FAIL " + t.payoutAddress + ": " + result);
                        failed++;
                    } else {
                        addLog("  SENT " + t.payoutAddress + " " +
                               std::to_string(perNode) + " DGB txid=" + result.substr(0, 16) + "...");
                        _db.recordPayout(t.payoutAddress, perNodeSat, result);
                        success++;
                    }
                }
                addLog("Payout complete: " + std::to_string(success) + " sent, " +
                        std::to_string(failed) + " failed");
            } else {
                addLog("Payout cancelled.");
            }
        } else if (ch == 'p' || ch == 'P') {
            // Payout preview (read-only).
            auto cfg = readPoolConfig(_configPath);
            auto targets = _db.getVerifiedPayoutTargets();
            std::string budgetMode;
            double spend = computeBudget(cfg, budgetMode);
            bool enabled = false;
            try { if (cfg.count("poolpayouts")) enabled = std::stoi(cfg["poolpayouts"]) != 0; } catch (...) {}

            addLog("--- Payout Preview ---");
            addLog("Verified nodes: " + std::to_string(targets.size()));
            char buf[64]; snprintf(buf, sizeof(buf), "%.4f", spend < 0 ? 0.0 : spend);
            addLog("Budget: " + std::string(buf) + " DGB (" + budgetMode + ")");
            if (!enabled) addLog("WARNING: poolpayouts=0 (payouts disabled). Set to 1 to enable.");
            if (targets.empty()) addLog("No eligible nodes. Nodes must be verified within 24h.");
            else if (spend < 0) addLog("Could not read wallet balance (check rpcuser/rpcpassword/rpcport).");
            else if (spend == 0) addLog("No budget. Set poolpayoutpercent=<%> (balance-based) or poolspendperperiod=<DGB>.");
            else {
                double perNode = spend / (double) targets.size();
                char perBuf[64]; snprintf(perBuf, sizeof(perBuf), "%.8f", perNode);
                addLog("Each node receives: " + std::string(perBuf) + " DGB");
                for (const auto& t: targets) {
                    addLog("  -> " + t.payoutAddress);
                }
            }
            double paid = _db.getPaidTotalDgb();
            char paidBuf[64]; snprintf(paidBuf, sizeof(paidBuf), "%.4f", paid);
            addLog("Total paid to date: " + std::string(paidBuf) + " DGB (" +
                    std::to_string(_db.getPaidCount()) + " transactions)");
        } else if (ch == 'e' || ch == 'E') {
            // Execute payout — with confirmation gate.
            auto cfg = readPoolConfig(_configPath);
            bool enabled = false;
            try { if (cfg.count("poolpayouts")) enabled = std::stoi(cfg["poolpayouts"]) != 0; } catch (...) {}
            std::string rpcUser = cfg.count("rpcuser") ? cfg["rpcuser"] : "";

            // Once-per-period spend guard: the budget is a *per period* amount,
            // so refuse a second payout until the period has elapsed. This stops
            // a double [E] press (or an itchy operator) from paying twice in a
            // row. Default period is 24h.
            int periodHours = 24;
            try { if (cfg.count("poolpayoutperiodhours")) periodHours = std::stoi(cfg["poolpayoutperiodhours"]); } catch (...) {}
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
            int64_t lastPayout = _db.getLastPayoutAt();

            auto targets = _db.getVerifiedPayoutTargets();

            // Cheap guards first; only compute the (RPC-backed) budget once we
            // know we'd actually pay.
            if (!enabled) {
                addLog("Cannot execute: poolpayouts=0 in pool.cfg. Set to 1 first.");
            } else if (targets.empty()) {
                addLog("Cannot execute: no verified nodes eligible for payout.");
            } else if (rpcUser.empty()) {
                addLog("Cannot execute: rpcuser not set in pool.cfg (needed for wallet RPC).");
            } else if (periodHours > 0 && lastPayout > 0 &&
                       (now - lastPayout) < (int64_t) periodHours * 3600) {
                int64_t remain = (int64_t) periodHours * 3600 - (now - lastPayout);
                addLog("Cannot execute: last payout was " + formatDuration(now - lastPayout) +
                       " ago. Next allowed in " + formatDuration(remain) +
                       " (poolpayoutperiodhours=" + std::to_string(periodHours) + ").");
            } else {
                std::string budgetMode;
                double spend = computeBudget(cfg, budgetMode);
                if (spend < 0) {
                    addLog("Cannot execute: could not read wallet balance (RPC error). Check rpcuser/rpcpassword/rpcport.");
                } else if (spend == 0) {
                    addLog("Cannot execute: no budget. Set poolpayoutpercent=<%> (balance-based) or poolspendperperiod=<DGB>.");
                } else {
                    double perNode = spend / (double) targets.size();
                    _pendingPerNode = perNode;
                    char buf[64]; snprintf(buf, sizeof(buf), "%.8f", perNode);
                    addLog("--- CONFIRM PAYOUT ---");
                    addLog("Budget: " + budgetMode);
                    addLog("Sending " + std::string(buf) + " DGB to each of " +
                            std::to_string(targets.size()) + " verified node(s)");
                    char totalBuf[64]; snprintf(totalBuf, sizeof(totalBuf), "%.8f", spend);
                    addLog("Total: " + std::string(totalBuf) + " DGB from your wallet");
                    addLog("Press Y to confirm, any other key to cancel");
                    _awaitingPayoutConfirm.store(true);
                }
            }
        } else if (ch == 'n' || ch == 'N') {
            addLog("Registered nodes: " + std::to_string(_db.countTotalNodes()));
        } else if (ch == 'a' || ch == 'A') {
            addLog("Permanent assets: " + std::to_string(_db.countPermanentAssets()) +
                   " across " + std::to_string(_db.countPermanentPages()) + " pages");
        } else if (ch == 'h' || ch == 'H' || ch == '?') {
            addLog("--- Help ---");
            addLog("Q = Quit   N = Node count   A = Asset count   H = This help");
            addLog("P = Preview payouts (read-only)   E = Execute payout (asks Y/N to confirm)");
            addLog("Verified row: nodes that passed the dial-back swarm/connect probe.");
            addLog("Failed out: nodes with 3+ consecutive probe failures (excluded from /nodes.json).");
            addLog("Payouts: 'disabled' until poolpayouts=1 is set in pool.cfg.");
        }
    }
#endif
}

void PoolDashboard::updateConsoleSize() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        _width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        _height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    _width = 120;
    _height = 40;
#endif
    if (_width < 40) _width = 40;
    if (_height < 15) _height = 15;
}

void PoolDashboard::render() {
    updateConsoleSize();

    std::ostringstream out;
    out << HIDE_CURSOR << CURSOR_HOME;

    const int w = _width;
    auto separator = [&]() { out << ERASE_LINE << std::string(w, '-') << "\n"; };

    // Centered title — same style as DigiAsset Core for Windows header.
    std::string title = "DigiAsset Pool Server for Windows " + std::string(VERSION_STRING) +
                        "  (Phase 1 - experimental)";
    int pad = (w - (int) title.size()) / 2;
    out << BOLD << FG_BRIGHT_WHITE << ERASE_LINE
        << std::string(pad > 0 ? pad : 0, ' ') << title << RESET << "\n";
    separator();

    // Status rows
    auto now = std::chrono::system_clock::now();
    int64_t uptime = std::chrono::duration_cast<std::chrono::seconds>(now - _startTime).count();
    int64_t oneHourAgo = std::chrono::duration_cast<std::chrono::seconds>(
                                 now.time_since_epoch()).count() - 3600;

    unsigned int totalNodes = _db.countTotalNodes();
    unsigned int activeNodes = _db.countNodesSeenSince(oneHourAgo);
    unsigned int permAssets = _db.countPermanentAssets();
    unsigned int permPages = _db.countPermanentPages();
    uint64_t requests = _server.getRequestCount();

    // ---- Aligned two-column header (same cell() pattern as DigiAssetCore) ----
    const int COL1_LABEL_W = 16;
    const int COL1_VALUE_W = 14;
    const int COL2_LABEL_W = 14;

    auto cell = [&](const std::string& label, const std::string& value,
                    const char* color, int labelWidth, int valueWidth) -> std::string {
        std::string result;
        std::string lp = label + ":" + std::string(std::max(0, labelWidth - (int) label.size() - 1), ' ');
        result += lp;
        result += color;
        result += value;
        result += RESET;
        if (valueWidth > 0) {
            int vpad = valueWidth - (int) value.size();
            if (vpad > 0) result += std::string(vpad, ' ');
        }
        return result;
    };

    // Dial-back verification counters.
    int64_t verifyCutoff = std::chrono::duration_cast<std::chrono::seconds>(
                                   now.time_since_epoch()).count() - 3600;
    unsigned int verifiedRecent = _db.countVerifiedSince(verifyCutoff);
    unsigned int failedOut = _db.countFailedOut();
    uint64_t probesAttempted = _verifier.getProbesAttempted();
    uint64_t probesSucceeded = _verifier.getProbesSucceeded();

    // Row: Listening | Requests
    out << ERASE_LINE << "  "
        << cell("Listening", "Port " + std::to_string(_server.getPort()), FG_GREEN, COL1_LABEL_W, COL1_VALUE_W)
        << cell("Requests", formatNumber(requests), FG_BRIGHT_WHITE, COL2_LABEL_W, 0)
        << "\n";

    // Row: Registered | Active (1h)
    out << ERASE_LINE << "  "
        << cell("Registered", std::to_string(totalNodes) + " nodes", FG_BRIGHT_WHITE, COL1_LABEL_W, COL1_VALUE_W)
        << cell("Active (1h)", std::to_string(activeNodes) + " nodes", FG_BRIGHT_WHITE, COL2_LABEL_W, 0)
        << "\n";

    // Row: Verified (1h) | Failed out
    out << ERASE_LINE << "  "
        << cell("Verified (1h)", std::to_string(verifiedRecent) + " nodes",
                verifiedRecent > 0 ? FG_GREEN : FG_YELLOW, COL1_LABEL_W, COL1_VALUE_W)
        << cell("Failed out", std::to_string(failedOut) + " nodes",
                failedOut > 0 ? FG_YELLOW : FG_BRIGHT_WHITE, COL2_LABEL_W, 0)
        << "\n";

    // Row: Probes | Permanent
    out << ERASE_LINE << "  "
        << cell("Probes", formatNumber(probesSucceeded) + " ok / " + formatNumber(probesAttempted),
                FG_BRIGHT_WHITE, COL1_LABEL_W, COL1_VALUE_W)
        << cell("Permanent", formatNumber(permAssets) + " / " + std::to_string(permPages) + " pages",
                FG_BRIGHT_WHITE, COL2_LABEL_W, 0)
        << "\n";

    // Row: Payout status + real ledger totals.
    {
        bool payoutsEnabled = _server.getPayoutsEnabled();
        double paidTotal = _db.getPaidTotalDgb();
        unsigned int paidCount = _db.getPaidCount();

        std::string payVal = payoutsEnabled ? "ENABLED" : "disabled";
        char paidBuf[64];
        snprintf(paidBuf, sizeof(paidBuf), "%.4f DGB (%u tx)", paidTotal, paidCount);
        out << ERASE_LINE << "  "
            << cell("Payouts", payVal, payoutsEnabled ? FG_GREEN : FG_YELLOW, COL1_LABEL_W, COL1_VALUE_W)
            << cell("Paid total", paidBuf, FG_BRIGHT_WHITE, COL2_LABEL_W, 0)
            << "\n";
    }

    // Row: Time | Uptime
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    tmBuf = *std::localtime(&t);
#endif
    char timeStr[16];
    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmBuf);

    out << ERASE_LINE << "  "
        << cell("Time", timeStr, FG_BRIGHT_WHITE, COL1_LABEL_W, COL1_VALUE_W)
        << cell("Uptime", formatDuration(uptime), FG_BRIGHT_WHITE, COL2_LABEL_W, 0)
        << "\n";

    separator();

    // Log area — show as many recent messages as fit in the terminal, then
    // key hints immediately after (no blank gap). Clear-to-end-of-screen
    // wipes any stale content below from prior longer renders or window
    // resizes.
    out << BOLD << ERASE_LINE << " Log:" << RESET << "\n";
    {
        // Available = terminal height - fixed header rows(10) - "Log:" row(1) - key hint row(1)
        int availableLogLines = _height - 12;
        if (availableLogLines < 5) availableLogLines = 5;

        std::lock_guard<std::mutex> lk(_logMutex);
        // Show the most recent N lines that fit.
        int startIdx = 0;
        if ((int) _logLines.size() > availableLogLines) {
            startIdx = (int) _logLines.size() - availableLogLines;
        }
        int printed = 0;
        for (int i = startIdx; i < (int) _logLines.size(); i++) {
            out << ERASE_LINE << "  " << _logLines[i] << "\n";
            printed++;
        }
        // Pad the log area with blank lines so the key hints row stays pinned
        // to the bottom of the window (same as ConsoleDashboard's help bar).
        for (int i = printed; i < availableLogLines; i++) {
            out << ERASE_LINE << "\n";
        }
    }

    // Key hints row — pinned at the bottom thanks to the padding above.
    // Phase 3 keys dimmed since they're placeholders.
    out << ERASE_LINE
        << " [Q] Quit  [N] Nodes  [A] Assets  [H] Help"
        << "  [P] Preview  [E] Execute payout";

    // Clear everything below: wipe stale lines from prior renders or
    // window resize, so the screen is clean below the key hints.
    out << ESC "J";

    std::cout << out.str() << std::flush;
}
