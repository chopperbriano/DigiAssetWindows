// main.cpp - entry point for the DigiAssetWindows node executable
// (DigiAssetWindows.exe). Runs the first-launch config wizard, connects to
// DigiByte Core, opens the local chain.db, starts the IPFS handler, Permanent Storage Pool list, RPC
// cache, chain analyzer, RPC server, and web server, then idles until a
// shutdown signal (Ctrl+C/SIGTERM or the dashboard's quit key) and tears down.

#include "AppMain.h"
#include "ChainAnalyzer.h"
#include "Config.h"
#include "ConsoleDashboard.h"
#include "Database.h"
#include "DigiAssetConstants.h"
#include "DigiByteCore.h"
#include "EventBroadcaster.h"
#include "IPFS.h"
#include "Log.h"
#include "RPC/Server.h"
#include "Version.h"
#include "WebServer.h"
#include "utils.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include "InstanceLock.h"
#ifdef _WIN32
#include <process.h>       // MSVC: getpid lives here as _getpid
#define getpid _getpid
#endif

// Global flag for graceful shutdown
static volatile std::sig_atomic_t g_shutdown = 0;
// Signal handler for SIGINT/SIGTERM: sets the shutdown flag that the various
// wait loops in main() poll so the node can exit cleanly. Async-signal-safe
// (only touches the volatile sig_atomic_t flag).
static void signalHandler(int signal) {
    g_shutdown = 1;
}

namespace {
    std::atomic<bool> shutdownRequested{false};
    extern "C" void handleShutdownSignal(int) {
        shutdownRequested = true; //signal safe: everything else happens on the main thread
    }
} // namespace

// Node process entry point. Wires up and starts every subsystem in dependency
// order, then blocks until shutdown is requested. Returns 0 on clean exit, -1 on
// fatal setup failures (bad config, database won't open), 1 on an uncaught
// exception. Note: the happy path never falls through to `return 0` - it calls
// std::exit(0) after teardown to kill the detached RPC/web-server threads.
// Takes argc/argv for --help.
int main(int argc, char* argv[]) {

  try {
    // The bootStrap struct is gone with the IPFS bootstrap image (upstream f37d61d).
    /*
     * Parse command line
     */
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        //--bootgen removed with the IPFS bootstrap image (upstream f37d61d). This fork's
        //fast-sync ships chain.db with its -wal/-shm from a cleanly stopped node
        //(snapshots/make-snapshot.ps1), so it never needed the compacted single-file build.
        if ((arg == "--help") || (arg == "-h")) {
            //fork branding: upstream prints "DigiAsset Core <ver>" / digiasset_core
            cout << getProductVersionString() << "\n"
                 << "Usage: DigiAssetWindows.exe [options]\n"
                 << "  --help      Show this message\n";
            return 0;
        } else {
            cerr << "Unknown option: " << arg << "\nTry --help\n";
            return 1;
        }
    }

    //make sure only one instance
    InstanceLock lock("digiasset_core");
    if (!lock.acquire()) {
        return 1;
    }
    std::cout << "Core application running. PID: " << getpid() << std::endl;

    // Handle Ctrl+C gracefully. NOTE: register our handlers AFTER
    // InstanceLock::acquire(), because acquire() installs its own SIGINT/SIGTERM
    // handlers (for lock-file cleanup). Registering ours last ensures our
    // graceful-shutdown flag (g_shutdown) wins so the dashboard/cursor teardown
    // below still runs.
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    ///Bootstrap images are gone - every node syncs from genesis now.  These are the images nodes
    ///used to pin, kept here only so a node that already has one lets go of it.  Every running node
    ///pinned the image whether or not it ever restored from one, so without this list they would
    ///each keep carrying several GB with no way to release it.  Safe to delete once enough time has
    ///passed that no node is still on a build old enough to have pinned them.
    const vector<string> oldBootstrapCIDs = {
            "QmVYaAEq5Whh1951RtRrBx1aFXiLuPoho4apRRa9tX6BDM",
            "QmaAHM9ZPGDWjW2Y5HhVzRVKAyrWofjzkN7pCW1juKgizU",
            "QmUUpXkcajwApumJ9KGz9nX7x1QmTQ4kTW4YzPc4HXqu4Z" //last official image, height 21505152
    };

    ///Files that every node keeps a copy of so they stay findable.  The storage pool only keeps asset
    ///metadata alive, so anything else on ipfs survives purely on whoever happens to still have it -
    ///and when that was one machine, the test fixtures below dropped to zero providers the moment it
    ///went offline.  Spreading them over every node costs each one a copy but means the test suite is
    ///never blocked on a single operator.
    ///Reviewers: only ever changed by a trusted party
    const vector<string> officialPinnedCIDs = {
            "QmNPyr5tkm48cUu5iMbReiM8GN8AW6PRpzUztPFadaxC8j", //tests/testFiles/assetTest.csv
            "QmUXQ2SMCvNAL4THgMm2g5vM4t6dBzj78ArnW9YBFmk81m"  //tests/testFiles/assetTest.db
    };

    ///Superseded entries from the list above, unpinned on start the same way oldBootstrapCIDs are.
    ///Without this a node that pinned one keeps carrying it forever with no way to let go
    const vector<string> retiredPinnedCIDs = {
            "QmVoawgnYej8TNwpBB7DtJ75KbrAB99k7f9VAWzqSLJBeX" //assetTest.db before it was vacuumed(243MB)
    };

    /*
     * Check if config exists and prompt user to make one if it doesn't
     */
    if (!utils::fileExists("config.cfg")) {
        Config config;
        cout << "Config file not found starting config wizard\n";

        //get DigiByte Core IP
        cout << "Is DigiByte Core running on this machine(Y/N)? ";
        bool localCore = utils::getAnswerBool();
        string rpcbind = "127.0.0.1";
        if (!localCore) {
            cout << "What is the IP address of DigiByte core? ";
            rpcbind = utils::getAnswerString(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)");
        }
        config.setString("rpcbind", rpcbind);

        //Get DigiByte Core Port
        cout << "What port is DigiByte Core running on(Default 14022)? ";
        int rpcport = utils::getAnswerInt(0, 65535);
        config.setInteger("rpcport", rpcport);

        //Get DigiByte Core username
        cout << "What is the username for DigiByte Core? ";
        string rpcuser = utils::getAnswerString();
        config.setString("rpcuser", rpcuser);

        //Get DigiByte Core password
        cout << "What is the password for DigiByte Core? ";
        string rpcpassword = utils::getAnswerString();
        config.setString("rpcpassword", rpcpassword);

        //todo check if above is correct

        cout << "Is IPFS running on this machine(Y/N)? ";
        bool localIPFS = utils::getAnswerBool();
        string ipfsPath = "http://localhost:5001/api/v0/";
        if (!localIPFS) {
            cout << "What is the path to the IPFS node? ";
            ipfsPath = utils::getAnswerString();
        }
        config.setString("ipfspath", ipfsPath);

        //todo check if above is correct

        //Get payout address. Pay to the local pool (psp0) and the active
        //networked pool (psp2 = DigiStamp on new nodes). psp1 is the deprecated
        //mctrivia slot and stays unsubscribed - matching the installer + dashboard.
        cout << "You will get paid for running this app.  What DigiByte address would you like to get paid to? ";
        string payout = utils::getAnswerString(R"(^(D|S)[1-9A-HJ-NP-Za-km-z]{25,34}|(dgb1)[qpzry9x8gf2tvdw0s3jn54khce6mua7l]{6,90}$)");
        config.setString("psp0payout", payout);
        config.setString("psp2payout", payout);

        //Get the Permanent Storage Pool server to join. This is the pool that
        //verifies your node and pays you for hosting. Keep the default in sync
        //with DEFAULT_POOL_BASE in src/PermanentStoragePool/pools/mctrivia.cpp.
        const string defaultPoolServer = "https://pool.digistamp.co";
        cout << "Which Permanent Storage Pool server should pay you for hosting?\n";
        cout << "Press Enter for the default (" << defaultPoolServer << ") or type another pool's URL: ";
        string poolServer = utils::getAnswerString();
        if (poolServer.empty()) poolServer = defaultPoolServer;
        config.setString("psp2server", poolServer);
        config.setBool("psp2subscribe", true);    // join the DigiStamp pool (index 2)
        config.setBool("psp1subscribe", false);   // legacy mctrivia pool stays off

        //check if user wants to store minimal information or everything
        cout << "Unpruned mode requires 100GB of storage.  Pruned mode requires 2 GB of storage.  Unless running a service like an explorer or wallet back end Pruned Mode is recommended.\n";
        cout << "Would you like to run in pruning mode(Y/N)? ";
        bool pruneMode = utils::getAnswerBool();
        config.setInteger("pruneage", pruneMode ? 5760 : -1);

        //get list of allowed rpc calls
        cout << "Do you wish to allow all RPC commands(Y/N)? ";
        bool allowAllRPC = utils::getAnswerBool();
        if (allowAllRPC) {
            config.setBool("rpcallow*", true);
        } else {
            cout << "Please list all RPC commands you would like to allow.  Press Enter on blank line when done";
            while (true) {
                string command = utils::getAnswerString();
                if (command.empty()) break;
                config.setBool("rpcallow" + command, true);
            }
        }

        //save config
        config.write("config.cfg");
    }

    /*
     * Start Log and Console Dashboard
     */
    Log* log = Log::GetInstance();
    Config config = Config("config.cfg");
    log->setMinLevelToScreen(static_cast<Log::LogLevel>(config.getInteger("logscreen", static_cast<int>(Log::INFO))));
    log->setMinLevelToFile(static_cast<Log::LogLevel>(config.getInteger("logfile", static_cast<int>(Log::WARNING))));

    // Set up the in-place console dashboard (config wizard is done, safe to take over screen)
    ConsoleDashboard dashboard;
    if (ConsoleDashboard::enableVT100()) {
        //passed as a sink so Log.cpp carries no dependency on ConsoleDashboard - the cli
        //links Log.cpp (upstream ee66c97) but not the dashboard.
        log->setDashboardSink([&dashboard](const std::string& m) { dashboard.addMessage(m); });
        dashboard.start();
    }

    /*
     * Refuse to run a config that does not mean what it says.
     * example.cfg writes the per pool options as psp#subscribe and so on, where # stands for the
     * pool number.  Copied over as is the # ends up part of the key name, so the line quietly does
     * nothing and the default applies - psp#subscribe=0 leaves the node subscribed.
     */
    vector<string> placeholderKeys = config.getPlaceholderKeys();
    for (const string& key: placeholderKeys) {
        string message = "config.cfg has \"" + key + "\" which is not a real config key.  The # in example.cfg stands for the pool number";
        if (key.substr(0, 4) == "psp#") {
            message += " - use psp0" + key.substr(4) + " or psp1" + key.substr(4);
        }
        message += ".  As written it does nothing";
        if (key == "psp#subscribe") message += ", so the pool stays subscribed";
        message += ".";
        log->addMessage(message, Log::CRITICAL);
    }
    if (!placeholderKeys.empty()) return 1;

    /*
     * Print starting message
     */
    log->addMessage("Starting " + getProductVersionString());

    /*
     * Get database filename from config (default "chain.db")
     */
    string dbFilename = config.getString("dbfilename", "chain.db");
    log->addMessage("Using database file: " + dbFilename);

    /*
     * Create AppMain
     */
    AppMain* main = AppMain::GetInstance();

    /*
     * Connect to core wall
     */

    DigiByteCore dgb;
    log->addMessage("Checking for DigiByte Core");
    dgb.setFileName("config.cfg");
    bool online = false;
    while (!online) {
        //connect to DigiByte Core
        try {
            dgb.makeConnection();
            log->addMessage("DigiByte Core Online");
            online = true;
        } catch (const DigiByteCore::exceptionCoreOffline& e) {
            log->addMessage("DigiByte Core Offline try again in 30 sec");
            online = false;
            this_thread::sleep_for(chrono::seconds(30)); //Don't hammer wallet
        } catch (const Config::exceptionConfigFileInvalid& e) {
            log->addMessage("DigiByte Core config values wrong in config file", Log::CRITICAL);
            return -1;
        } catch (const std::exception& e) {
            //Any other startup hiccup (wallet still coming up, momentary RPC
            //error) - keep waiting rather than aborting the node.
            log->addMessage("DigiByte Core not ready yet (" + string(e.what()) + ") try again in 30 sec");
            online = false;
            this_thread::sleep_for(chrono::seconds(30));
        }
        if (g_shutdown != 0) return 0; //allow Ctrl+C while waiting for the wallet
    }
    main->setDigiByteCore(&dgb);

    /*
     * Refuse to index against a node that predates DigiDollar.
     *
     * DigiDollar activated as a soft fork, so an old node still follows the same chain and still
     * answers every RPC we make - it just cannot validate DigiDollar and reports nothing useful
     * about those transactions.  Nothing about the connection tells us this, so it has to be
     * checked explicitly or we would silently index a chain we cannot read properly.
     */
    int nodeVersion = dgb.getNodeVersion();
    if (nodeVersion == 0) {
        log->addMessage("Could not determine DigiByte Core version - getnetworkinfo did not answer. "
                        "DigiAsset Core requires v9.26.5 or newer.",
                        Log::CRITICAL);
        return -1;
    }
    if (nodeVersion < DigiByteCore::MINIMUM_NODE_VERSION) {
        //render 92605 as 9.26.5 so the message names a version the operator can actually download
        auto renderVersion = [](int v) {
            return to_string(v / 10000) + "." + to_string((v / 100) % 100) + "." + to_string(v % 100);
        };
        log->addMessage("DigiByte Core " + renderVersion(nodeVersion) + " is too old.  DigiAsset Core "
                        "requires v" + renderVersion(DigiByteCore::MINIMUM_NODE_VERSION) + " or newer "
                        "because it indexes DigiDollar, which activated at block 23,869,440 and "
                        "cannot be validated by earlier releases.  Upgrade the node and restart.",
                        Log::CRITICAL);
        return -1;
    }
    log->addMessage("DigiByte Core version " + to_string(nodeVersion) + " accepted");

    /*
     * Get wallet version
     */
    DigiByteCore::WalletVersion walletVersion = dgb.coreVersion();
    if (walletVersion < DigiByteCore::WalletVersion::v9) {
        log->addMessage("DigiByte Core wallet " + DigiByteCore::walletVersionName(walletVersion) +
                                " is no longer supported.  DigiAsset Core requires a v9 wallet or "
                                "newer - upgrade DigiByte Core and restart.",
                        Log::CRITICAL);
        return -1;
    }

    /**
     * Connect to Database
     * Make sure it is initialized with correct database
     */
    Database* db = nullptr;
    try {
        log->addMessage("Loading Database");
        db = new Database(dbFilename);
        auto compatibleWalletVersion = db->getCompatibleWalletVersion();
        if ((compatibleWalletVersion>0) && (compatibleWalletVersion != walletVersion)) {
            cout << "██ ███    ██  ██████  ██████  ███    ███ ██████   █████  ████████ ██ ██████  ██      ███████ \n"
                    "██ ████   ██ ██      ██    ██ ████  ████ ██   ██ ██   ██    ██    ██ ██   ██ ██      ██      \n"
                    "██ ██ ██  ██ ██      ██    ██ ██ ████ ██ ██████  ███████    ██    ██ ██████  ██      █████   \n"
                    "██ ██  ██ ██ ██      ██    ██ ██  ██  ██ ██      ██   ██    ██    ██ ██   ██ ██      ██      \n"
                    "██ ██   ████  ██████  ██████  ██      ██ ██      ██   ██    ██    ██ ██████  ███████ ███████ \n"
                    "                                                                                             \n"
                    " DigiByte Core Wallet " << DigiByteCore::walletVersionName(walletVersion) << " detected. \n"
                    " Database compatible with " << DigiByteCore::walletVersionName(compatibleWalletVersion) << "\n"
                    " Change core version or delete chain.db and restart\n";
            return -1;
        }
        main->setDatabase(db);
    } catch (const std::exception& e) {
        // chain.db is unusable (corrupt or half-built). It is 100% re-derivable
        // from the blockchain, so instead of dying and making the user delete it
        // by hand, rename the bad file aside and rebuild ONCE from scratch.
        log->addMessage(std::string("chain.db is unusable (") + e.what() +
                                ") - renaming it aside and rebuilding from scratch.",
                        Log::WARNING);
        std::string stamp = std::to_string(static_cast<long long>(time(nullptr)));
        const char* suffixes[] = {"", "-wal", "-shm"};
        for (const char* suffix: suffixes) {
            std::string from = std::string("chain.db") + suffix;
            if (utils::fileExists(from)) {
                std::rename(from.c_str(), ("chain.db.corrupt-" + stamp + suffix).c_str());
            }
        }
        try {
            db = new Database("chain.db"); // fresh, clean build
            main->setDatabase(db);
            log->addMessage("Rebuilt a fresh chain.db (old one saved as chain.db.corrupt-*). "
                            "The node will re-scan assets from the blockchain.",
                            Log::WARNING);
        } catch (const std::exception& e2) {
            log->addMessage(std::string("Could not rebuild chain.db: ") + e2.what(), Log::CRITICAL);
            return -1;
        }
    }

    /**
     * Connect to IPFS
     */
    log->addMessage("Starting IPFS handler");
    IPFS ipfs("config.cfg");
    main->setIPFS(&ipfs);
    for (const auto& cid: officialPinnedCIDs) {
        ipfs.pin(cid);
    }
    for (const auto& cid: oldBootstrapCIDs) {
        ipfs.unpin(cid);
    }
    for (const auto& cid: retiredPinnedCIDs) {
        ipfs.unpin(cid);
    }

    /**
     * Connect to Permanent Storage Pools
     */
    PermanentStoragePoolList* psp;
    try {
        log->addMessage("Starting Permanent Storage Pool handler");
        psp = new PermanentStoragePoolList("config.cfg");
        main->setPermanentStoragePoolList(psp);
        // One-shot plain-English explanation of the current PSP reality, so a
        // first-time Windows user reading the log understands what their node
        // is actually doing and why no DGB is showing up.
        log->addMessage("PSP info: this node is storing DigiAsset pool files for the network.");
        log->addMessage("PSP info: payments from mctrivia's pool are currently unavailable - the");
        log->addMessage("PSP info: pool operator's payment service is offline and operator is");
        log->addMessage("PSP info: unreachable. Your node is still contributing useful storage");
        log->addMessage("PSP info: to the DigiByte ecosystem.");
    } catch (const DigiByteException& e) {
        log->addMessage("Error PSP payout address not set and couldn't auto create one", Log::CRITICAL);
        return 0;
    }

    /**
     * Start RPC Cache
     */
    log->addMessage("Starting RPC Cache");
    RPC::Cache rpcCache;
    main->setRpcCache(&rpcCache);

    /**
     * Start Chain Analyzer
     */
    log->addMessage("Starting Chain Analyzer");
    ChainAnalyzer analyzer;
    analyzer.loadConfig();
    main->setChainAnalyzer(&analyzer);

    /**
     * Start RPC Server. start() is non-blocking — it spawns the Server's own
     * accept thread — so we no longer wrap it in a detached thread here. The
     * shared_ptr is held at function scope so the Server outlives shutdown
     * (stop() joins its accept thread before we tear the process down).
     */
    std::shared_ptr<RPC::Server> rpcServer;
    {
        try {
            log->addMessage("Starting RPC Server");
            rpcServer = std::make_shared<RPC::Server>();
            main->setRpcServer(rpcServer.get());
            rpcServer->start();
        } catch (const std::exception& e) {
            log->addMessage(std::string("RPC server failed: ") + e.what(), Log::CRITICAL);
        }
    }

    /**
     * Start event stream (TCP newline-delimited JSON events; config eventport, 0 disables)
     * and the Web Server.
     *
     * In bootgen mode both stay off, along with the RPC server above, so nothing
     * outside this process can touch chain.db while we are building an image to
     * share. (Upstream 1ddf933 gates the RPC server here; ours starts earlier as
     * a shared_ptr, so it is gated at its own site instead.)
     */
    WebServer webServer("config.cfg");
    {
        EventBroadcaster::GetInstance()->start(config.getInteger("eventport", 14025),
                                               config.getString("eventbind", "127.0.0.1"));

        try {
            log->addMessage("Starting Web Server");
            main->setWebServer(&webServer);
            webServer.start();
        } catch (const std::exception& e) {
            log->addMessage(std::string("Web server failed: ") + e.what(), Log::CRITICAL);
        }
    }

    /**
     * Start Chain Analyzer
     */
    try {
        analyzer.start();
    } catch (const std::exception& e) {
        log->addMessage(std::string("Chain Analyzer start failed: ") + e.what(), Log::CRITICAL);
    }
    // Upstream's wait loop is NOT taken here: we already have our own that also
    // honours the dashboard's [Q] quit and our g_shutdown signal handler
    // (registered earlier, per INTEGRATION-mctrivia.md §5). The bootgen
    // stop-when-synced condition that used to live in this loop went with the
    // IPFS bootstrap image (upstream f37d61d).

    // Wait for shutdown signal (Ctrl+C or Q key)
    while (!g_shutdown && !dashboard.quitRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Graceful shutdown. Order matters: stop EVERYTHING that could still touch the
    // database (RPC server + web console handle read requests; the analyzer writes)
    // BEFORE flushing the WAL, so no other thread races the checkpoint or the
    // process teardown. Then flush the WAL into chain.db so the file is complete on
    // its own.
    log->addMessage("Shutting down...");
    // Stop the RPC server first: it's the main thing that services DB reads on
    // its worker threads, and its stop() closes the acceptor to unblock cleanly.
    if (auto* rpc = main->getRpcServerIfSet()) { try { rpc->stop(); } catch (...) {} }
    // Then the web console — it also services (throttled) DB reads. WebServer::stop()
    // wakes its blocked accept() with a throwaway loopback connection, so this joins
    // promptly instead of hanging as an earlier version did.
    try { webServer.stop(); } catch (...) {}
    analyzer.stop();                                                                    // joins the analyzer thread
    EventBroadcaster::GetInstance()->stop();
    // Stop the networked Permanent Storage Pool threads (keepalive/fetcher) and the
    // IPFS worker BEFORE the WAL checkpoint. BOTH write chain.db (the fetcher pins
    // CIDs -> Database::addIPFSJob; the IPFS worker drains the job queue), so
    // letting them run into walCheckpoint()/std::exit races the checkpoint (the
    // "Database ... SQL command failed" seen on shutdown) and leaves the IPFS
    // worker touching statics that are being destroyed at exit (use-after-free).
    if (auto* pspList = main->getPermanentStoragePoolListIfSet()) { try { pspList->stopAll(); } catch (...) {} }
    try { ipfs.stop(); } catch (...) {}                                                 // joins the IPFS worker thread
    db->walCheckpoint();                                                                // flush WAL into chain.db (now no other thread writes)
    log->addMessage("Shutdown complete");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Force exit — any remaining detached threads won't hold the process open
    dashboard.stop();
    std::cout << "\033[?25h" << std::flush;

    std::exit(0);


    return 0;

  } catch (const std::exception& e) {
    std::cerr << "\nFATAL: " << e.what() << std::endl;
    std::cerr << "Press Enter to exit..." << std::endl;
    std::cin.get();
    return 1;
  } catch (...) {
    std::cerr << "\nFATAL: Unknown error" << std::endl;
    std::cerr << "Press Enter to exit..." << std::endl;
    std::cin.get();
    return 1;
  }
}
