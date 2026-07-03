#include "AppMain.h"
#include "ChainAnalyzer.h"
#include "Config.h"
#include "ConsoleDashboard.h"
#include "Database.h"
#include "DigiByteCore.h"
#include "IPFS.h"
#include "Log.h"
#include "RPC/Server.h"
#include "Version.h"
#include "WebServer.h"
#include "utils.h"
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>

// Global flag for graceful shutdown
static volatile std::sig_atomic_t g_shutdown = 0;
static void signalHandler(int signal) {
    g_shutdown = 1;
}


int main() {
  // Handle Ctrl+C gracefully
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  try {
    ///When updating bootstrap image change both values.   Reviewers make sure this value is only ever changed by trusted party
    const vector<string> oldBootstrapCIDs = {"QmVYaAEq5Whh1951RtRrBx1aFXiLuPoho4apRRa9tX6BDM"};
    const string officialBootstrapCID = "QmaAHM9ZPGDWjW2Y5HhVzRVKAyrWofjzkN7pCW1juKgizU";
    const unsigned int officialBootStrapHeight = 19256623;

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

        //Get payout address
        cout << "You will get paid for running this app.  What DigiByte address would you like to get paid to? ";
        string payout = utils::getAnswerString(R"(^(D|S)[1-9A-HJ-NP-Za-km-z]{25,34}|(dgb1)[qpzry9x8gf2tvdw0s3jn54khce6mua7l]{6,90}$)");
        config.setString("psp0payout", payout);
        config.setString("psp1payout", payout);

        //Get the Permanent Storage Pool server to join. This is the pool that
        //verifies your node and pays you for hosting. Keep the default in sync
        //with DEFAULT_POOL_BASE in src/PermanentStoragePool/pools/mctrivia.cpp.
        const string defaultPoolServer = "https://pool.digistamp.co";
        cout << "Which Permanent Storage Pool server should pay you for hosting?\n";
        cout << "Press Enter for the default (" << defaultPoolServer << ") or type another pool's URL: ";
        string poolServer = utils::getAnswerString();
        if (poolServer.empty()) poolServer = defaultPoolServer;
        config.setString("psp1server", poolServer);

        //check if user wants to store minimal information or everything
        cout << "Unpruned mode requires 100GB of storage.  Pruned mode requires 2 GB of storage.  Unless running a service like an explorer or wallet back end Pruned Mode is recommended.\n";
        cout << "Would you like to run in pruning mode(Y/N)? ";
        bool pruneMode = utils::getAnswerBool();
        bool bootstrap = false;
        if (pruneMode) {
            cout << "Would you like to bootstrap the database from IPFS(Y) or sync from the begining(N)? ";
            bootstrap = utils::getAnswerBool();
        }
        config.setInteger("pruneage", pruneMode ? 5760 : -1);
        config.setBool("bootstrapchainstate", bootstrap);

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
        log->setDashboard(&dashboard);
        dashboard.start();
    }

    /*
     * Print starting message
     */
    log->addMessage("Starting " + getProductVersionString());

    /*
     * Predownload database files if config files allow and database missing
     */
    unsigned int pauseHeight = 0;
    if (                                                   //download bootstrap if all of the above are true
            config.getBool("bootstrapchainstate", true) && //if bootstrap is allowed by config(default true)
            !config.getBool("storenonassetutxo", false) && //if we are not storing the non asset utxo
            !utils::fileExists("chain.db")) {              //if the chain database does not yet exist
        log->addMessage("Bootstraping Database.  This may take a while depending on how faster your internet is.");
        IPFS ipfs("config.cfg", false);
        //The bootstrap download depends on IPFS being up and the CID being
        //reachable.  Rather than let a timeout abort the whole node, retry a few
        //times (IPFS may still be starting) and, if it still won't come, carry on
        //WITHOUT the bootstrap - the chain simply syncs from scratch instead.
        bool bootstrapped = false;
        for (unsigned int attempt = 1; attempt <= 5 && !bootstrapped; attempt++) {
            try {
                ipfs.downloadFile(officialBootstrapCID, "chain.db", true);
                bootstrapped = true;
            } catch (const std::exception& e) {
                std::remove("chain.db"); //discard any partial download before retrying
                log->addMessage(
                        "Bootstrap download failed (is IPFS running/reachable?): " + string(e.what()) +
                        " - attempt " + to_string(attempt) + " of 5, waiting 30s...");
                if (attempt < 5) this_thread::sleep_for(chrono::seconds(30));
            }
        }
        if (bootstrapped) {
            pauseHeight = officialBootStrapHeight+2;
        } else {
            std::remove("chain.db"); //ensure no partial file is left for the DB to open
            log->addMessage("Bootstrap unavailable - continuing without it; the chain will sync from scratch (slower, but the node won't crash).");
        }
    }

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
        }
    }
    main->setDigiByteCore(&dgb);

    //make sure if we predownloaded data from ipfs that the wallet is synced past the point image was syned to
    if (pauseHeight > 0) {
        while (dgb.getBlockCount() < pauseHeight) {
            log->addMessage("DigiByte Core Syncing try again in 2 minutes");
            this_thread::sleep_for(chrono::minutes(2)); //Don't hammer wallet
        }
    }

    /**
     * Connect to Database
     * Make sure it is initialized with correct database
     */
    Database* db;
    try {
        log->addMessage("Loading Database");
        db = new Database("chain.db");
        main->setDatabase(db);
    } catch (const Database::exceptionFailedToOpen& e) {
        log->addMessage("Database could not be opened", Log::CRITICAL);
        return -1;
    }

    /**
     * Connect to IPFS
     */
    log->addMessage("Starting IPFS handler");
    IPFS ipfs("config.cfg");
    main->setIPFS(&ipfs);
    ipfs.pin(officialBootstrapCID);
    for (const auto& cid: oldBootstrapCIDs) {
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
     * Start RPC Server in its own thread so it doesn't block the main thread
     */
    try {
        log->addMessage("Starting RPC Server");
        std::shared_ptr<RPC::Server> server = std::make_shared<RPC::Server>();
        main->setRpcServer(server.get());
        std::thread rpcThread([server] {
            server->start();
        });
        rpcThread.detach();
    } catch (const std::exception& e) {
        log->addMessage(std::string("RPC server failed: ") + e.what(), Log::CRITICAL);
    }

    /**
     * Start Web Server
     */
    WebServer webServer("config.cfg");
    try {
        log->addMessage("Starting Web Server");
        main->setWebServer(&webServer);
        webServer.start();
    } catch (const std::exception& e) {
        log->addMessage(std::string("Web server failed: ") + e.what(), Log::CRITICAL);
    }

    /**
     * Start Chain Analyzer
     */
    try {
        analyzer.start();
    } catch (const std::exception& e) {
        log->addMessage(std::string("Chain Analyzer start failed: ") + e.what(), Log::CRITICAL);
    }

    // Wait for shutdown signal (Ctrl+C or Q key)
    while (!g_shutdown && !dashboard.quitRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Graceful shutdown
    log->addMessage("Shutting down...");
    analyzer.stop();
    log->addMessage("Shutdown complete");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Force exit — detached threads (RPC server, web server) won't hold process
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