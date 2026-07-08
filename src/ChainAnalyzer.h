//
// Created by mctrivia on 02/02/23.
//
// ChainAnalyzer — the node's blockchain indexing engine.
//
// Runs on its own worker thread (extends Threaded). It walks the DigiByte
// chain block-by-block from the local DigiByte Core RPC, hands each transaction
// to DigiByteTransaction for DigiAsset parsing, and persists the results into
// the Database. It also handles fork rewinds (rolling back when the local chain
// reorganizes) and periodic pruning of historical data to keep the DB small.
// Sync progress (getSync()/getSyncHeight()) is what the ConsoleDashboard and
// RPC report. This is the core of the DigiAssetWindows.exe node deployable.
//

#ifndef DIGIBYTECORE_CHAINANALYZER_H
#define DIGIBYTECORE_CHAINANALYZER_H

#define DIGIBYTE_BLOCK1_HASH "4da631f2ac1bed857bd968c67c913978274d8aabed64ab2bcebc1665d7f4d3a0";

#define PRUNE_INTERVAL_DIVISOR 8
//higher numbers will prune more often.  Do not set higher then 100

#include "Database.h"
#include "DigiByteCore.h"
#include "Threaded.h"
#include <cmath>
#include <thread>

/**
 * Blockchain analyzer/indexer worker. Owns the sync loop that reads blocks from
 * DigiByte Core, extracts DigiAsset data, writes it to the Database, and prunes
 * old history. State is exposed as an int (SYNCED / STOPPED / INITIALIZING /
 * REWINDING / BUSY, or a negative "blocks behind" count while catching up).
 * Config (prune settings, what to store/pin) loads from and saves to config.cfg.
 */
class ChainAnalyzer : public Threaded {
public:
    //constructor/destructor
    explicit ChainAnalyzer();
    ~ChainAnalyzer();

    //load/save config
    void setFileName(const std::string& fileName);
    void saveConfig();
    void loadConfig();
    void loadFake(unsigned int databaseHeight, int syncLevel);

    //set config values
    void setPruneAge(int age); //-1 disable pruning
    void setPruneExchangeHistory(bool shouldPrune);
    void setPruneUTXOHistory(bool shouldPrune);
    void setPruneVoteHistory(bool shouldPrune);
    void setStoreNonAssetUTXO(bool shouldStore);

    //running state modifiers
    void restart(); //erases all data and starts syncing over from block 1

    //SYNCING is anything <0 where the number is how many blocks behind it is
    static const int SYNCED = 0;
    static const int STOPPED = 1;
    static const int INITIALIZING = 2;
    static const int REWINDING = 3;
    static const int BUSY = 4;

    //get state
    int getSync() const;
    unsigned int getSyncHeight() const;

    /**
     * Builds a formatted multi-line table of accumulated timing stats (total,
     * average, and count) for the three hot paths: transaction processing,
     * transaction saving, and address-cache clearing. Read-only; used for
     * performance diagnostics.
     * @return the formatted table as a string
     */
    std::string printProfilingInfo() {
        long long totalDuration = _processTransactionRunTime;
        unsigned int transactions = _processTransactionRunCount;
        long long avgDuration = transactions > 0 ? totalDuration / transactions : 0;

        std::ostringstream oss;
        oss << std::right << std::setw(30) << "ChainAnalyzer Tx Process"
            << std::setw(20) << totalDuration
            << std::setw(20) << avgDuration
            << std::setw(20) << transactions << std::endl;

        totalDuration = _saveTransactionRunTime;
        transactions = _saveTransactionRunCount;
        avgDuration = transactions > 0 ? totalDuration / transactions : 0;

        oss << std::right << std::setw(30) << "ChainAnalyzer Tx Save"
            << std::setw(20) << totalDuration
            << std::setw(20) << avgDuration
            << std::setw(20) << transactions << std::endl;

        totalDuration = _clearAddressCacheRunTime;
        transactions = _clearAddressCacheRunCount;
        avgDuration = transactions > 0 ? totalDuration / transactions : 0;

        oss << std::right << std::setw(30) << "ChainAnalyzer Cache Clean"
            << std::setw(20) << totalDuration
            << std::setw(20) << avgDuration
            << std::setw(20) << transactions << std::endl;

        return oss.str();
    }

private:
    std::string _configFileName = "config.cfg";

    //thread overrides (called by the Threaded base class on its worker thread)
    void startupFunction() override; //recover DB from last shutdown, set initial height/state
    void mainFunction() override;    //the sync loop body (rewind + sync); re-entered on error
    void shutdownFunction() override;//mark state STOPPED on thread exit

    //config functions
    void resetConfig();
    unsigned int pruneMax(unsigned int height);
    bool shouldPruneExchangeHistory() const;
    bool shouldPruneUTXOHistory() const;
    bool shouldPruneVoteHistory() const;
    bool shouldStoreNonAssetUTXO() const;

    //state(for defaults see resetConfig() )
    int _state = STOPPED;
    int _height;
    std::string _nextHash;

    //config variables(chain data)
    int _pruneAge; //number of blocks to keep for roll back protection(-1 don't prune, default is 1 day)
    int _pruneInterval;
    bool _pruneExchangeHistory; //if true prune "exchange"
    bool _pruneUTXOHistory;     //if true prune "utxos"
    bool _pruneVoteHistory;     //if true prune "votes
    bool _storeNonAssetUTXOs;   //if false won't bother storing NonAsset UTXOS
    bool _verifyDatabaseWrite;  //if set to false will write without checking
    bool _showAllBlockSyncTime; //if true will not collapse blocks of 100 together when behind
    bool _pipelineSync;         //if true, prefetch the next block on a bg thread during deep bulk sync
    // Fast (relaxed-durability) pragmas are used for the whole genesis->tip
    // catch-up regardless of _verifyDatabaseWrite; if the operator wants durable
    // writes they're restored once at the tip. Tracks that one-time restore.
    bool _writeVerificationRestored = false;
    bool _hasRunOnce = false;   //tracks if mainFunction has been called before (for error recovery)
    int _errorCount = 0;        //consecutive error count for backoff/stop

    //config variable(meta data) - need to be static or make entire thing singleton.  decided to make static
    static unsigned int _pinAssetIcon;
    static unsigned int _pinAssetDescription;
    static unsigned int _pinAssetExtra;
    static unsigned int _pinAssetPermanent;
    static std::map<std::string, int> _pinAssetExtraMimeTypes;

    //time stats
    long long _processTransactionRunTime = 0;
    unsigned int _processTransactionRunCount = 0;
    long long _saveTransactionRunTime = 0;
    unsigned int _saveTransactionRunCount = 0;
    long long _clearAddressCacheRunTime = 0;
    unsigned int _clearAddressCacheRunCount = 0;

    //phases functions
    void phaseRewind();//detect a fork at the current height and roll the DB back to the fork point
    void phaseSync();  //main catch-up/tip-follow loop: fetch, process, and store each block
    void phasePrune(); //drop history older than the prune window when it's time to prune

    //process sub functions
    void processTX(const std::string& txid, unsigned int height);//parse one tx, store it, invalidate caches

    friend class Database; //so database can modify state
public:
    /*
   ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
   ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
   █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
   ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
   ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
   ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
    */
    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "Chain Analyzer: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    class exceptionAlreadyPruned : public exception {
    public:
        explicit exceptionAlreadyPruned()
            : exception("Already been pruned") {}
    };
};


#endif //DIGIBYTECORE_CHAINANALYZER_H
