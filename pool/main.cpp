//
// DigiAssetPoolServer - optional companion exe for DigiAssetCore for Windows.
//
// Implements mctrivia's pool wire protocol so the win.31+ C++ client can
// register payout addresses and fetch the permanent asset list from a pool
// this operator controls (rather than mctrivia's server, which has been
// returning HTTP 500 on payout-related endpoints since ~July 2024).
//
// Phase 1 scope:
//   - Minimal HTTP endpoints: /permanent/<page>.json, /list/<floor>.json,
//     /keepalive, /nodes.json, /map.json, /bad.json
//   - SQLite pool.db for persistent state (nodes, permanent asset list,
//     payouts ledger shell, pool config)
//   - First-run snapshot bootstraps the permanent list from mctrivia's
//     current /permanent/0..23.json so existing clients work immediately
//   - Minimal TUI dashboard showing listening port, registered nodes,
//     asset count, pending/paid totals, uptime
//   - Operator-facing menu keys (Q/N/A/P/E/H) for status + placeholder
//     payout approval commands
//
// Phase 2: dial-back verification of registered peers.
// Phase 3: operator-approved payout batch distribution via local DigiByte
//          Core RPC.
//

#include "PoolDashboard.h"
#include "PoolDatabase.h"
#include "PoolServer.h"
#include "PoolVerifier.h"
#include "CurlHandler.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    // ---- Shutdown signal -------------------------------------------------
    std::atomic<bool> g_shutdown{false};

    void signalHandler(int) {
        g_shutdown.store(true);
    }

    // ---- Tiny config.cfg reader ------------------------------------------
    //
    // Shares the same simple "key=value\n" format the main exe uses for
    // config.cfg. We don't need the full Config class from src/ for the
    // pool exe — this is ~15 lines.
    std::map<std::string, std::string> readConfig(const std::string& path) {
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

    int readConfigInt(const std::map<std::string, std::string>& cfg,
                      const std::string& key, int defaultValue) {
        auto it = cfg.find(key);
        if (it == cfg.end()) return defaultValue;
        try { return std::stoi(it->second); }
        catch (...) { return defaultValue; }
    }

    // ---- First-run snapshot ----------------------------------------------
    //
    // If pool.db has zero permanent_assets rows, fetch mctrivia's current
    // /permanent/<page>.json pages and import them so new clients see the
    // same canonical list they used to get from mctrivia. This is a
    // one-time bootstrap; after it runs once, future operator-added assets
    // are layered on top.
    //
    // We walk pages until we hit the error envelope (mctrivia's "page not
    // populated" response) or a hard HTTP error. Pages 0..23 are populated
    // as of 2026-04-11 but don't hardcode the count.
    void firstRunSnapshot(PoolDatabase& db, PoolDashboard& dash) {
        if (db.hasPermanentData()) return; // already populated

        dash.addLog("First run: snapshotting mctrivia's /permanent pages...");

        const std::string base = "https://ipfs.digiassetx.com/permanent/";
        unsigned int totalAssets = 0;
        unsigned int page = 0;
        const unsigned int maxPages = 100; // safety cap

        for (; page < maxPages; page++) {
            std::string body;
            try {
                body = CurlHandler::get(base + std::to_string(page) + ".json", 15000);
            } catch (...) {
                dash.addLog("  page " + std::to_string(page) + ": HTTP error, stopping");
                break;
            }

            // Cheap "is this an error envelope?" sniff. Mctrivia's server
            // returns {"error":"Unexpected Error"} for pages past the end.
            if (body.find("\"error\"") != std::string::npos &&
                body.find("\"changes\"") == std::string::npos) {
                dash.addLog("  page " + std::to_string(page) + ": end of list");
                break;
            }

            // Find the "daily" value (string).
            std::string daily = "0";
            {
                size_t pos = body.find("\"daily\"");
                if (pos != std::string::npos) {
                    pos = body.find(':', pos);
                    if (pos != std::string::npos) {
                        pos++;
                        while (pos < body.size() && std::isspace((unsigned char) body[pos])) pos++;
                        if (pos < body.size() && body[pos] == '"') {
                            pos++;
                            size_t end = body.find('"', pos);
                            if (end != std::string::npos) daily = body.substr(pos, end - pos);
                        }
                    }
                }
            }

            // Find done flag.
            bool done = false;
            {
                size_t pos = body.find("\"done\"");
                if (pos != std::string::npos) {
                    pos = body.find(':', pos);
                    if (pos != std::string::npos) {
                        pos++;
                        while (pos < body.size() && std::isspace((unsigned char) body[pos])) pos++;
                        if (body.compare(pos, 4, "true") == 0) done = true;
                    }
                }
            }

            // Walk the "changes" object: { "<assetId>-<txHash>": [cids], ... }
            // Minimal hand parsing since we can't depend on jsoncpp from the
            // pool exe (would need to add it to the link line). The changes
            // block always looks like: "changes":{"<key>":["<cid>",...],...}
            unsigned int pageAssets = 0;
            {
                size_t changesPos = body.find("\"changes\"");
                if (changesPos != std::string::npos) {
                    size_t objStart = body.find('{', changesPos);
                    if (objStart != std::string::npos) {
                        size_t pos = objStart + 1;
                        int depth = 1;
                        while (pos < body.size() && depth > 0) {
                            while (pos < body.size() && body[pos] != '"' &&
                                   body[pos] != '}' && body[pos] != '{') pos++;
                            if (pos >= body.size()) break;
                            if (body[pos] == '}') { depth--; pos++; continue; }
                            if (body[pos] == '{') { depth++; pos++; continue; }

                            // body[pos] == '"' — start of a key
                            size_t keyStart = pos + 1;
                            size_t keyEnd = body.find('"', keyStart);
                            if (keyEnd == std::string::npos) break;
                            std::string fullKey = body.substr(keyStart, keyEnd - keyStart);
                            pos = keyEnd + 1;

                            // Split "<assetId>-<txHash>"
                            size_t dash = fullKey.find('-');
                            std::string assetId, txHash;
                            if (dash == std::string::npos) {
                                assetId = fullKey;
                            } else {
                                assetId = fullKey.substr(0, dash);
                                txHash  = fullKey.substr(dash + 1);
                            }

                            // Skip to the array
                            while (pos < body.size() && body[pos] != '[') pos++;
                            if (pos >= body.size() || body[pos] != '[') break;
                            pos++;

                            // Read CID strings until ']'
                            while (pos < body.size() && body[pos] != ']') {
                                while (pos < body.size() && body[pos] != '"' && body[pos] != ']') pos++;
                                if (pos >= body.size() || body[pos] == ']') break;
                                size_t cidStart = pos + 1;
                                size_t cidEnd = body.find('"', cidStart);
                                if (cidEnd == std::string::npos) break;
                                std::string cid = body.substr(cidStart, cidEnd - cidStart);
                                if (!cid.empty()) {
                                    try {
                                        db.insertPermanentAsset(assetId, txHash, cid, page);
                                        pageAssets++;
                                    } catch (...) {}
                                }
                                pos = cidEnd + 1;
                                while (pos < body.size() && (body[pos] == ',' ||
                                       std::isspace((unsigned char) body[pos]))) pos++;
                            }
                            if (pos < body.size() && body[pos] == ']') pos++;
                            while (pos < body.size() && (body[pos] == ',' ||
                                   std::isspace((unsigned char) body[pos]))) pos++;
                        }
                    }
                }
            }

            try { db.setPermanentPageDone(page, done, daily); }
            catch (...) {}

            totalAssets += pageAssets;
            dash.addLog("  page " + std::to_string(page) + ": imported " +
                        std::to_string(pageAssets) + " entries" +
                        (done ? " (done)" : " (still growing)"));
        }

        db.setConfig("snapshotCompleted", "1");
        dash.addLog("Snapshot complete: " + std::to_string(totalAssets) +
                    " entries across " + std::to_string(page) + " pages");
    }
}

int main(int /*argc*/, char** /*argv*/) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Config: read pool.cfg from cwd. Minimal keys: poolport, pooldbpath.
    // Everything else comes from defaults.
    auto cfg = readConfig("pool.cfg");
    int port = readConfigInt(cfg, "poolport", 14028);
    std::string dbPath = cfg.count("pooldbpath") ? cfg["pooldbpath"] : "pool.db";
    // Payouts are OFF by default. The operator flips this to 1 in pool.cfg
    // ONLY after Phase 3 is wired up, the pool's payout wallet is funded,
    // and the dry-run commands have been tested. Until then, clients see
    // "registered (no payouts yet)" instead of "active", so nobody thinks
    // they're earning DGB when they aren't.
    bool payoutsEnabled = readConfigInt(cfg, "poolpayouts", 0) != 0;
    // IPFS HTTP API base the verifier uses for swarm-connect dial-back.
    // Defaults to the same localhost Kubo that DigiAssetCore itself talks to.
    std::string ipfsApi = cfg.count("ipfspath") ? cfg["ipfspath"]
                                                : "http://localhost:5001/api/v0/";

    std::cout << "DigiAsset Pool Server starting...\n";
    std::cout << "  db: " << dbPath << "\n";
    std::cout << "  port: " << port << "\n";
    std::cout << "  ipfsApi: " << ipfsApi << "\n";
    std::cout << "  poolpayouts: " << (payoutsEnabled ? "ENABLED" : "disabled (Phase 1 default)") << "\n";
    if (payoutsEnabled) {
        std::cout << "\n  !!! WARNING: poolpayouts=1 is set but Phase 3 automated payout\n"
                  << "  !!! distribution has not shipped yet. Connected clients WILL report\n"
                  << "  !!! 'Payment: active' but no DGB will actually move unless you manually\n"
                  << "  !!! send it. Set poolpayouts=0 until Phase 3 lands.\n\n";
    }

    // Open / initialize the database.
    PoolDatabase* db = nullptr;
    try {
        db = new PoolDatabase(dbPath);
    } catch (const std::exception& e) {
        std::cerr << "FATAL: failed to open pool database: " << e.what() << std::endl;
        return 1;
    }

    // Construct the server. Binds the listen socket in the ctor; if bind
    // fails we abort before touching the dashboard so the error is visible.
    PoolServer* server = nullptr;
    try {
        server = new PoolServer(*db, (unsigned int) port);
        server->setPayoutsEnabled(payoutsEnabled);

        // Donation/treasury stats page (GET /pool/stats.json). Optional — if
        // the donation address or RPC creds are unset the endpoint reports
        // zeros. Uses the SAME DigiByte Core RPC creds the payout path uses.
        std::string donationAddr = cfg.count("pooldonationaddress") ? cfg["pooldonationaddress"] : "";
        std::string rpcUser = cfg.count("rpcuser") ? cfg["rpcuser"] : "";
        std::string rpcPass = cfg.count("rpcpassword") ? cfg["rpcpassword"] : "";
        int rpcPort = readConfigInt(cfg, "rpcport", 14022);
        std::string explorerPrefix = cfg.count("poolexplorertxprefix")
                                         ? cfg["poolexplorertxprefix"]
                                         : "https://digiexplorer.info/tx/";
        // Esplora-style address API used to read the (external) treasury
        // address balance + received total for the stats page.
        std::string addrApiPrefix = cfg.count("pooladdrapiprefix")
                                        ? cfg["pooladdrapiprefix"]
                                        : "https://digiexplorer.info/api/address/";
        server->setWalletInfo(donationAddr, rpcUser, rpcPass, rpcPort, explorerPrefix, addrApiPrefix);
    } catch (const std::exception& e) {
        std::cerr << "FATAL: failed to start pool server on port " << port << ": " << e.what() << std::endl;
        std::cerr << "Is another process already listening on " << port << "? Check netstat -ano." << std::endl;
        delete db;
        return 1;
    }

    // PoolVerifier runs a background dial-back loop against registered
    // nodes using the local IPFS HTTP API. Phase 2 scope: verifies peers
    // are reachable via swarm-connect; doesn't yet verify they're serving
    // specific CIDs. See pool/PoolVerifier.h for why.
    PoolVerifier verifier(*db, ipfsApi);

    // Dashboard takes over the console. Passes the config path so [P] and
    // [E] key handlers can re-read pool.cfg on demand (allows adjusting
    // poolspendperperiod between payouts without a restart).
    PoolDashboard dashboard(*db, *server, verifier, "pool.cfg");
    if (!PoolDashboard::enableVT100()) {
        std::cerr << "Warning: could not enable VT100 on this console, dashboard may look garbled\n";
    }

    dashboard.start();
    dashboard.addLog("Pool database ready: " + dbPath);
    dashboard.addLog("HTTP server listening on port " + std::to_string(port));
    dashboard.addLog("Verifier using IPFS API: " + ipfsApi);

    // First-run snapshot runs after dashboard is up so the progress lines
    // show in the log area. Blocks the main thread for ~5-10 seconds.
    try {
        firstRunSnapshot(*db, dashboard);
    } catch (const std::exception& e) {
        dashboard.addLog(std::string("First-run snapshot error: ") + e.what());
    }

    server->start();
    verifier.start();
    dashboard.addLog("Accepting connections");
    dashboard.addLog("Verifier running (swarm-connect dial-back every 60s)");
    dashboard.addLog("Ready. Press [H] for keys, [Q] to quit.");

    // Main shutdown wait loop.
    while (!g_shutdown.load() && !dashboard.quitRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    dashboard.addLog("Shutting down...");
    verifier.stop();
    server->stop();
    dashboard.stop();
    delete server;
    delete db;

    std::cout << "\n\033[?25hDigiAsset Pool Server stopped.\n";
    return 0;
}
