//
// Created by mctrivia on 30/01/23.
//

// Database.h
// Declares the Database class - the node's single SQLite-backed store of all
// chain-derived state. It holds the analyzed blockchain data the node produces
// while scanning DigiByte: assets (DigiAssets and their metadata/CID/rules),
// UTXOs, blocks, exchange rates, KYC records, votes, DigiByte-Domain mappings,
// the IPFS pin/download job queue, Permanent Storage Pool membership (pspFiles/
// pspAssets), unknown op_returns, and encrypted keys. It also computes periodic
// algo/address statistics. Every query is a pre-compiled Statement (see
// Database_Statement.h) accessed through a LockedStatement for thread safety.
// This one class underpins both deployables: the node uses it as its chain
// index, and the pool server uses the Permanent/PSP tables to track pinned CIDs.

#ifndef SHA256_LENGTH
#define SHA256_LENGTH 32
#endif

#ifndef STRINGIZE
#define STRINGIZE(x) #x
#define STRINGIZE_VALUE_OF(x) STRINGIZE(x)
#endif




#ifndef DIGIBYTECORE_DATABASECHAIN_H
#define DIGIBYTECORE_DATABASECHAIN_H

#define DIGIBYTECORE_DATABASE_CHAIN_WATCH_MAX 20


#include "BitIO.h"
#include "Blob.h"
#include "Database_LockedStatement.h"
#include "Database_Statement.h"
#include "DigiAssetRules.h"
#include "DigiAssetTypes.h"
#include "DigiByteCore.h"
#include "IPFS.h"
#include "KYC.h"
#include <future>
#include <iomanip>
#include <mutex>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <vector>

// A deferred CREATE INDEX request: its index name and the SQL to build it.
// Queued during setup and applied lazily by executePerformanceIndex().
struct PerformanceIndex {
    std::string name;
    std::string command;
};
// One row of per-time-window address statistics (see updateAddressStats).
struct AddressStats {
    unsigned int time;            //time block start
    unsigned int created;         //number of addresses created for the first time
    unsigned int used;            //number of addresses used during time block
    unsigned int withAssets;      //number of addresses with assets
    unsigned int over0;           //address count with any DGB
    unsigned int over1;           //address count with at least 1 DGB
    unsigned int over1k;          //address count with at least 1000 DGB
    unsigned int over1m;          //address count with at least 1000000 DGB
    unsigned int quantumInsecure; //number of addresses a quantum computer could steel from
    unsigned int total;           //total number of addresses that have ever existed up to this point
};

// One row of per-time-window, per-mining-algo block statistics (see updateAlgoStats).
struct AlgoStats {
    unsigned int time;   //time block start
    unsigned int algo;   //algo number
    unsigned int blocks; //number of blocks created with that algo
    double difficultyMin;
    double difficultyMax;
    double difficultyAvg;
};

// Tally of votes cast to a given address (for a voting asset).
struct VoteCount {
    std::string address;
    uint64_t count;
};

// Quantity held of a single asset variant, keyed by its assetIndex.
struct AssetCount {
    unsigned int assetIndex;
    uint64_t count;
};

// Minimal block summary (height/hash/time/algo) used by list queries.
struct BlockBasics {
    unsigned int height;
    std::string hash;
    unsigned int time;
    unsigned int algo;
};




// The node's SQLite chain-analysis store. Owns the sqlite3 connection and a
// large set of pre-prepared Statements (one per query, declared below). Public
// methods are grouped by table (assets, blocks, exchange, kyc, utxos, votes,
// ipfs jobs, DigiByte-Domains, Permanent/PSP, stats). Most methods run a bound
// LockedStatement and throw a Database::exception subclass on failure. A few
// caches (flags, exchange-watch addresses, master domain ids, and a two-
// generation non-asset-UTXO cache) live in RAM to avoid DB/RPC round-trips.
class Database {
private:
    sqlite3* _db = nullptr;

    // In-memory cache of non-asset UTXOs to avoid RPC fallback in getAssetUTXO
    // Uses two generations: when active map exceeds MAX_UTXO_CACHE, it becomes
    // the old map and a fresh one is created. Lookups check both.
    // This bounds memory to ~2x MAX_UTXO_CACHE entries.
    struct NonAssetUtxoInfo {
        std::string address;
        uint64_t digibyte;
    };
    static const size_t MAX_UTXO_CACHE = 1000000; // ~100MB at ~100 bytes/entry
    std::unordered_map<std::string, NonAssetUtxoInfo> _utxoCacheActive;
    std::unordered_map<std::string, NonAssetUtxoInfo> _utxoCacheOld;
    Statement _stmtCheckFlag;
    Statement _stmtSetFlag;
    Statement _stmtGetBlockHeight;
    Statement _stmtInsertBlock;
    Statement _stmtGetBlockHash;
    Statement _stmtCreateUTXO;
    Statement _stmtSpendUTXO;
    Statement _stmtGetUTXOHeight;
    Statement _stmtIsWatchAddress;
    Statement _stmtAddWatchAddress;
    Statement _stmtGetSpendingAddress;
    Statement _stmtAddExchangeRate;
    Statement _stmtAddKYC;
    Statement _stmtRevokeKYC;
    Statement _stmtPruneUTXOs;
    Statement _stmtExchangeRatesAtHeight;
    Statement _stmtPruneExchangeRate;
    Statement _stmtGetVoteCountAtHeight;
    Statement _stmtPruneVote;
    Statement _stmtAddVote;
    Statement _stmtGetVoteCount;
    Statement _stmtGetAssetUTXO;
    Statement _stmtGetAssetHolders;
    Statement _stmtAddAsset;
    Statement _stmtUpdateAsset;
    Statement _stmtGetAssetIndex;
    Statement _stmtGetAssetIndexOnUTXO;
    Statement _stmtGetAssetIDsOrderedByHeight;
    Statement _stmtGetLastAssetIssued;
    Statement _stmtGetHeightAssetCreated;
    Statement _stmtGetAssetRules;
    Statement _stmtGetAsset;
    Statement _stmtGetKYC;
    Statement _stmtGetValidExchangeRate;
    Statement _stmtGetCurrentExchangeRate;
    Statement _stmtGetNextIPFSJob;
    Statement _stmtSetIPFSPauseSync;
    Statement _stmtClearNextIPFSJob_a;
    Statement _stmtClearNextIPFSJob_b;
    Statement _stmtInsertIPFSJob;
    Statement _stmtClearIPFSPause;
    Statement _stmtSetIPFSLockSync;
    Statement _stmtSetIPFSLockJob;
    Statement _stmtSetIPFSPauseJob;
    Statement _stmtGetDomainAssetId;
    Statement _stmtAddDomain;
    Statement _stmtRevokeDomain;
    Statement _stmtSetDomainMasterAssetId_a;
    Statement _stmtSetDomainMasterAssetId_b;
    Statement _stmtGetPermanentPaid;
    Statement _stmtRemoveNonReachable;
    Statement _stmtInsertPermanent;
    Statement _stmtRepinAssets;
    Statement _stmtRepinPermanentSpecific;
    Statement _stmtAddAssetToPool;
    Statement _stmtIsAssetInPool;
    Statement _stmtIsAssetInAPool;
    Statement _stmtPSPFileList;
    Statement _stmtPSPFindBadAsset;
    Statement _stmtPSPDeleteBadAsset;
    Statement _stmtDeletePermanent;
    Statement _stmtIsInPermanent;
    Statement _stmtNumberOfIPFSJobs;
    Statement _stmtGetTotalAssetCounta;
    Statement _stmtGetTotalAssetCountb;
    Statement _stmtGetOriginalAssetCounta;
    Statement _stmtGetOriginalAssetCountb;
    Statement _stmtGetAssetIssuanceTXIDs;
    Statement _stmtGetAssetTxHistorya;
    Statement _stmtGetAssetTxHistoryb;
    Statement _stmtGetAddressTxHistory;
    Statement _stmtGetAssetCreateByAddress;
    Statement _stmtGetAddressHoldings;
    Statement _stmtGetValidUTXO;
    Statement _stmtGetAddressChangesDuringPeriod;
    Statement _stmtGetLastBlocks;
    Statement _stmtInsertUnknown;
    Statement _stmtGetUnknowns;
    Statement _stmtDeleteFromUnknowns;
    Statement _stmtInsertEncryptedKey;
    Statement _stmtGetEncryptedKey;

public:
    // Builds a formatted table (as a string) of every prepared statement's
    // accumulated lock time, average time per use, and use count. Diagnostic
    // helper for spotting slow queries; does not touch the database itself.
    std::string printProfilingInfo() {
        // Header
        std::ostringstream oss;
        oss << std::right << std::setw(30) << "Statement Name"
            << std::setw(20) << "Total Time (us)"
            << std::setw(20) << "Time/Transaction (us)"
            << std::setw(20) << "Transactions" << std::endl;
        oss << std::string(90, '-') << std::endl; // Separator
        std::string result = oss.str();

        // Print info for each statement
        result += printStatementInfo("_stmtCheckFlag", _stmtCheckFlag);
        result += printStatementInfo("_stmtSetFlag", _stmtSetFlag);
        result += printStatementInfo("_stmtGetBlockHeight", _stmtGetBlockHeight);
        result += printStatementInfo("_stmtInsertBlock", _stmtInsertBlock);
        result += printStatementInfo("_stmtGetBlockHash", _stmtGetBlockHash);
        result += printStatementInfo("_stmtCreateUTXO", _stmtCreateUTXO);
        result += printStatementInfo("_stmtSpendUTXO", _stmtSpendUTXO);
        result += printStatementInfo("_stmtGetUTXOHeight", _stmtGetUTXOHeight);
        result += printStatementInfo("_stmtIsWatchAddress", _stmtIsWatchAddress);
        result += printStatementInfo("_stmtAddWatchAddress", _stmtAddWatchAddress);
        result += printStatementInfo("_stmtGetSpendingAddress", _stmtGetSpendingAddress);
        result += printStatementInfo("_stmtAddExchangeRate", _stmtAddExchangeRate);
        result += printStatementInfo("_stmtAddKYC", _stmtAddKYC);
        result += printStatementInfo("_stmtRevokeKYC", _stmtRevokeKYC);
        result += printStatementInfo("_stmtPruneUTXOs", _stmtPruneUTXOs);
        result += printStatementInfo("_stmtExchangeRatesAtHeight", _stmtExchangeRatesAtHeight);
        result += printStatementInfo("_stmtPruneExchangeRate", _stmtPruneExchangeRate);
        result += printStatementInfo("_stmtGetVoteCountAtHeight", _stmtGetVoteCountAtHeight);
        result += printStatementInfo("_stmtPruneVote", _stmtPruneVote);
        result += printStatementInfo("_stmtAddVote", _stmtAddVote);
        result += printStatementInfo("_stmtGetVoteCount", _stmtGetVoteCount);
        result += printStatementInfo("_stmtGetAssetUTXO", _stmtGetAssetUTXO);
        result += printStatementInfo("_stmtGetAssetHolders", _stmtGetAssetHolders);
        result += printStatementInfo("_stmtAddAsset", _stmtAddAsset);
        result += printStatementInfo("_stmtUpdateAsset", _stmtUpdateAsset);
        result += printStatementInfo("_stmtGetAssetIndex", _stmtGetAssetIndex);
        result += printStatementInfo("_stmtGetAssetIndexOnUTXO", _stmtGetAssetIndexOnUTXO);
        result += printStatementInfo("_stmtGetAssetIDsOrderedByHeight", _stmtGetAssetIDsOrderedByHeight);
        result += printStatementInfo("_stmtGetLastAssetIssued", _stmtGetLastAssetIssued);
        result += printStatementInfo("_stmtGetHeightAssetCreated", _stmtGetHeightAssetCreated);
        result += printStatementInfo("_stmtGetAssetRules", _stmtGetAssetRules);
        result += printStatementInfo("_stmtGetAsset", _stmtGetAsset);
        result += printStatementInfo("_stmtGetKYC", _stmtGetKYC);
        result += printStatementInfo("_stmtGetValidExchangeRate", _stmtGetValidExchangeRate);
        result += printStatementInfo("_stmtGetCurrentExchangeRate", _stmtGetCurrentExchangeRate);
        result += printStatementInfo("_stmtGetNextIPFSJob", _stmtGetNextIPFSJob);
        result += printStatementInfo("_stmtSetIPFSPauseSync", _stmtSetIPFSPauseSync);
        result += printStatementInfo("_stmtClearNextIPFSJob_a", _stmtClearNextIPFSJob_a);
        result += printStatementInfo("_stmtClearNextIPFSJob_b", _stmtClearNextIPFSJob_b);
        result += printStatementInfo("_stmtInsertIPFSJob", _stmtInsertIPFSJob);
        result += printStatementInfo("_stmtClearIPFSPause", _stmtClearIPFSPause);
        result += printStatementInfo("_stmtSetIPFSLockSync", _stmtSetIPFSLockSync);
        result += printStatementInfo("_stmtSetIPFSLockJob", _stmtSetIPFSLockJob);
        result += printStatementInfo("_stmtSetIPFSPauseJob", _stmtSetIPFSPauseJob);
        result += printStatementInfo("_stmtGetDomainAssetId", _stmtGetDomainAssetId);
        result += printStatementInfo("_stmtAddDomain", _stmtAddDomain);
        result += printStatementInfo("_stmtRevokeDomain", _stmtRevokeDomain);
        result += printStatementInfo("_stmtSetDomainMasterAssetId_a", _stmtSetDomainMasterAssetId_a);
        result += printStatementInfo("_stmtSetDomainMasterAssetId_b", _stmtSetDomainMasterAssetId_b);
        result += printStatementInfo("_stmtGetPermanentPaid", _stmtGetPermanentPaid);
        result += printStatementInfo("_stmtRemoveNonReachable", _stmtRemoveNonReachable);
        result += printStatementInfo("_stmtInsertPermanent", _stmtInsertPermanent);
        result += printStatementInfo("_stmtRepinAssets", _stmtRepinAssets);
        result += printStatementInfo("_stmtRepinPermanentSpecific", _stmtRepinPermanentSpecific);
        result += printStatementInfo("_stmtAddAssetToPool", _stmtAddAssetToPool);
        result += printStatementInfo("_stmtIsAssetInPool", _stmtIsAssetInPool);
        result += printStatementInfo("_stmtIsAssetInAPool", _stmtIsAssetInAPool);
        result += printStatementInfo("_stmtPSPFileList", _stmtPSPFileList);
        result += printStatementInfo("_stmtPSPFindBadAsset", _stmtPSPFindBadAsset);
        result += printStatementInfo("_stmtPSPDeleteBadAsset", _stmtPSPDeleteBadAsset);
        result += printStatementInfo("_stmtDeletePermanent", _stmtDeletePermanent);
        result += printStatementInfo("_stmtIsInPermanent", _stmtIsInPermanent);
        result += printStatementInfo("_stmtNumberOfIPFSJobs", _stmtNumberOfIPFSJobs);
        result += printStatementInfo("_stmtGetTotalAssetCounta", _stmtGetTotalAssetCounta);
        result += printStatementInfo("_stmtGetTotalAssetCountb", _stmtGetTotalAssetCountb);
        result += printStatementInfo("_stmtGetOriginalAssetCounta", _stmtGetOriginalAssetCounta);
        result += printStatementInfo("_stmtGetOriginalAssetCountb", _stmtGetOriginalAssetCountb);
        result += printStatementInfo("_stmtGetAssetIssuanceTXIDs", _stmtGetAssetIssuanceTXIDs);
        result += printStatementInfo("_stmtGetAssetTxHistorya", _stmtGetAssetTxHistorya);
        result += printStatementInfo("_stmtGetAssetTxHistoryb", _stmtGetAssetTxHistoryb);
        result += printStatementInfo("_stmtGetAddressTxHistory", _stmtGetAddressTxHistory);
        result += printStatementInfo("_stmtGetAssetCreateByAddress", _stmtGetAssetCreateByAddress);
        result += printStatementInfo("_stmtGetAddressHoldings", _stmtGetAddressHoldings);
        result += printStatementInfo("_stmtGetValidUTXO", _stmtGetValidUTXO);
        result += printStatementInfo("_stmtGetLastBlocks", _stmtGetLastBlocks);
        result += printStatementInfo("_stmtInsertUnknown", _stmtInsertUnknown);
        result += printStatementInfo("_stmtGetUnknowns", _stmtGetUnknowns);
        result += printStatementInfo("_stmtDeleteFromUnknowns", _stmtDeleteFromUnknowns);
        result += printStatementInfo("_stmtInsertEncryptedKey", _stmtInsertEncryptedKey);
        result += printStatementInfo("_stmtGetEncryptedKey", _stmtGetEncryptedKey);
        return result;
    }

    // Formats one row of the profiling table for the named statement: total
    // lock time, average per transaction, and transaction count.
    std::string printStatementInfo(const std::string& name, const Statement& stmt) {
        long long totalDuration = stmt.getTotalLockDuration();
        int transactions = stmt.getLockCount();
        long long avgDuration = transactions > 0 ? totalDuration / transactions : 0;

        std::ostringstream oss;
        oss << std::right << std::setw(30) << name
            << std::setw(20) << totalDuration
            << std::setw(20) << avgDuration
            << std::setw(20) << transactions << std::endl;

        return oss.str();
    }

private:
    std::vector<PerformanceIndex> _performanceIndexes;
    int _transactionDepth = 0;

    //locks
    std::mutex _mutexGetNextIPFSJob;
    std::mutex _mutexRemoveIPFSJob;
    std::mutex _mutexUpdateStats;

    void buildTables(unsigned int dbVersionNumber = 0);
    void initializeClassValues();
    bool isStructurallySound();   // PRAGMA quick_check - false if the file is corrupt/malformed
    void dropAllTables();         // DROP every schema table (used to rebuild an incoherent chain.db)

    //flag table
    int getFlagInt(const std::string& flag);
    void setFlagInt(const std::string& flag, int state);
    std::map<std::string, int> _flagState;

    //exchangeWatch table
    std::vector<std::string> _exchangeWatchAddresses;

    //TestHelpers
    static int defaultCallback(void* NotUsed, int argc, char** argv, char** azColName);

    //helpers
    static int executeSqliteStepWithRetry(sqlite3_stmt* stmt, int maxRetries = 3, int sleepDurationMs = 100);
    void executeSQLStatement(const std::string& query, const std::exception& errorToThrowOnFail);
    void handleSpecialErrors(unsigned int lineNumber = 0);

    //ipfs ram db values
    std::vector<std::pair<std::string, uint64_t>> _ipfsCurrentlyPaused;
    static std::map<std::string, IPFSCallbackFunction> _ipfsCallbacks;
    // Guards every read/write of _ipfsCallbacks. The map is touched by up to 10
    // IPFS worker threads plus the analyzer thread; without one shared lock a
    // concurrent erase vs find/operator[] corrupts the tree. (audit M5)
    static std::mutex _ipfsCallbacksMutex;

    //DigiBYte Domain ram values
    std::vector<std::string> _masterDomainAssetId = {};

    //private stats functions
    void addStatsPerformanceIndexes();
    unsigned int getStatsEndTime(unsigned int timeFrame, unsigned int beginHeight);
    unsigned int getStatsEndBlockHeight(unsigned int timeFrame, unsigned int endTime);
    void updateAlgoStats(unsigned int timeFrame, unsigned int endTime, unsigned int startHeight, unsigned int endHeight);
    void updateAddressStats(unsigned int timeFrame, unsigned int endTime, unsigned int startHeight, unsigned int endHeight);

public:
    struct exchangeRateHistoryValue {
        unsigned int height;
        std::string address;
        unsigned char index;
        double value;
    };

    //constructor
    Database(const std::string& newFileName = "chain.db");
    ~Database();

    //performance related
    void startTransaction();
    void endTransaction();
    void
    disableWriteVerification(); //on power failure not all commands may be written.  If using need to check at startup
    // Restores durable, shareable write settings (synchronous=FULL, on-disk
    // journal, non-exclusive lock) after a fast catch-up. Keeps the read-side
    // perf pragmas (cache/temp/mmap) since they don't affect durability.
    void enableWriteVerification();

    //indexes
    bool indexExists(const std::string& indexName);
    // Queues (but does not yet create) a covering index on the given table over
    // the listed columns. Each column may include a sort direction (e.g.
    // "height DESC"); the index name is derived from the table and column names.
    // No-op if an index of that derived name already exists. The queued index is
    // materialized later by executePerformanceIndex(), so building it never
    // blocks the caller. Variadic in the column list.
    template<typename... Columns>
    void addPerformanceIndex(const std::string& table, Columns... cols) {
        // Generate the index name
        std::stringstream indexName;
        indexName << "idx_" << table;
        auto appendWithUnderscore = [&indexName](const std::string& col) {
            indexName << "_" << col.substr(0, col.find(' ')); // Use only the column name part for the index name
        };
        std::initializer_list<int> dummy = {(appendWithUnderscore(cols), 0)...};
        static_cast<void>(dummy); // Avoid unused variable warning

        // Check if index exists
        if (indexExists(indexName.str())) return;

        // Create SQL command using a lambda
        std::stringstream indexCommand;
        indexCommand << "CREATE INDEX " << indexName.str() << " ON " << table << "(";
        bool first = true;
        auto appendColumn = [&indexCommand, &first](const std::string& col) {
            if (!first) {
                indexCommand << ", ";
            }
            // Check if col contains a space, indicating a sort direction is specified
            size_t spacePos = col.find(' ');
            if (spacePos != std::string::npos) {
                // Column name and direction are specified
                indexCommand << "\"" << col.substr(0, spacePos) << "\" " << col.substr(spacePos + 1);
            } else {
                // Only column name is specified
                indexCommand << "\"" << col << "\"";
            }
            first = false;
        };
        std::initializer_list<int> dummy2 = {(appendColumn(cols), 0)...};
        static_cast<void>(dummy2); // Avoid unused variable warning
        indexCommand << ");";

        // Store the index creation command for later use
        PerformanceIndex pi;
        pi.name = indexName.str();
        pi.command = indexCommand.str();
        _performanceIndexes.emplace_back(pi);
    }
    void executePerformanceIndex(int& state);
    // True while deferred performance indexes are still waiting to be built.
    bool performanceIndexesPending() const { return !_performanceIndexes.empty(); }

    //reset database
    void reset(); //used in case of roll back exceeding pruned history

    //assets table
    uint64_t addAsset(const DigiAsset& asset);
    DigiAsset getAsset(uint64_t assetIndex, uint64_t amount = 0);
    uint64_t getAssetIndex(const std::string& assetId, const std::string& txid = "", unsigned int vout = 0);
    std::vector<uint64_t> getAssetIndexes(const std::string& assetId);
    std::vector<AssetBasics> getAssetsIssued(unsigned int amount, unsigned int offset);
    std::vector<AssetBasics> getLastAssetsIssued(unsigned int amount = std::numeric_limits<unsigned int>::max(), unsigned int startAsset = std::numeric_limits<unsigned int>::max());

    //assets table not to be used on assets that may have more than one assetIndex
    DigiAssetRules getRules(const std::string& assetId);
    unsigned int getAssetHeightCreated(const std::string& assetId, unsigned int backupHeight, uint64_t& assetIndex);

    //block table
    void insertBlock(uint height, const std::string& hash, unsigned int time, unsigned char algo, double difficulty);
    std::string getBlockHash(uint height);
    uint getBlockHeight();
    void clearBlocksAboveHeight(uint height);
    std::vector<BlockBasics> getLastBlocks(unsigned int limit, unsigned int start = std::numeric_limits<unsigned int>::max());

    //exchange table
    void addExchangeRate(const std::string& address, unsigned int index, unsigned int height, double exchangeRate);
    void pruneExchange(unsigned int height);
    double getAcceptedExchangeRate(const ExchangeRate& rate, unsigned int height);
    double getCurrentExchangeRate(const ExchangeRate& rate);
    std::vector<exchangeRateHistoryValue> getExchangeRatesAtHeight(unsigned int height);

    //exchange watch table
    bool isWatchAddress(const std::string& address);
    void addWatchAddress(const std::string& address);

    //flag table
    int getBeenPrunedExchangeHistory(); //-1 = never, above=height which anything below may be pruned
    int getBeenPrunedUTXOHistory();     //-1 = never, above=height which anything below may be pruned
    int getBeenPrunedVoteHistory();     //-1 = never, above=height which anything below may be pruned
    bool getBeenPrunedNonAssetUTXOHistory();
    void setBeenPrunedExchangeHistory(int height); //-1 = never
    void setBeenPrunedUTXOHistory(int height);     //-1 = never
    void setBeenPrunedVoteHistory(int height);     //-1 = never
    void setBeenPrunedNonAssetUTXOHistory(bool state);

    //kyc table
    void
    addKYC(const std::string& address, const std::string& country, const std::string& name, const std::string& hash,
           unsigned int height);
    void revokeKYC(const std::string& address, unsigned int height);
    KYC getAddressKYC(const std::string& address);

    //utxos table
    void createUTXO(const AssetUTXO& value, unsigned int heightCreated, bool assetIssuance);
    void spendUTXO(const std::string& txid, unsigned int vout, unsigned int heightSpent, const std::string& spentTXID);
    std::pair<unsigned int, unsigned int> getUTXOHeight(const std::string& txid, unsigned int vout);
    std::string getSendingAddress(const std::string& txid, unsigned int vout);
    void pruneUTXO(unsigned int height);

    //asset stats
    uint64_t getAssetCountOnChain();
    // Highest assetIndex ever assigned in the assets table. This is the
    // right number for paginating callers (explorer browse, index range
    // enumeration) because listlastassets/listassets are keyed on
    // assetIndex -- NOT on distinct assetId count, which getAssetCountOnChain
    // returns and which silently drops reissuances.
    uint64_t getMaxAssetIndex();

    //utxo table asset related
    AssetUTXO getAssetUTXO(const std::string& txid, unsigned int vout, unsigned int height = 0);
    std::vector<AssetHolder> getAssetHolders(uint64_t assetIndex);
    std::vector<AssetHolder> getAssetHolders(std::string assetId);
    uint64_t getTotalAssetCount(uint64_t assetIndex);        //returns total count of specific variant
    uint64_t getTotalAssetCount(const std::string& assetId); //returns total count of specific asset(sum of all variants)
    uint64_t getOriginalAssetCount(uint64_t assetIndex);
    uint64_t getOriginalAssetCount(const std::string& assetId);
    std::vector<IssuanceBasics> getAssetIssuanceTXIDs(const std::string& assetId);
    std::vector<std::string> getAssetTxHistory(uint64_t assetIndex);
    std::vector<std::string> getAssetTxHistory(const std::string& assetId);

    //utxo table address related
    std::vector<AssetUTXO> getAddressUTXOs(const std::string& address, unsigned int minConfirms = 0, unsigned int maxConfirms = std::numeric_limits<unsigned int>::max());
    std::vector<std::string> getAddressTxList(const std::string& address, unsigned int minHeight = 1, unsigned int maxHeight = std::numeric_limits<unsigned int>::max(), unsigned int limit = 1000);
    std::vector<uint64_t> getAssetsCreatedByAddress(const std::string& address);
    std::vector<AssetCount> getAddressHoldings(const std::string& address);

    //vote table
    void addVote(const std::string& address, unsigned int assetIndex, uint64_t count, unsigned int height);
    void pruneVote(unsigned int height);
    std::vector<VoteCount> getVoteCounts(unsigned int assetIndex);

    //IPFS table
    static void registerIPFSCallback(const std::string& callbackSymbol, const IPFSCallbackFunction& callback);
    void getNextIPFSJob(unsigned int& jobIndex, std::string& cid, std::string& sync, std::string& extra,
                        unsigned int& maxSleep, IPFSCallbackFunction& callback);
    void pauseIPFSSync(unsigned int jobIndex, const std::string& sync, unsigned int pauseLengthInSeconds = 3600);
    void removeIPFSJob(unsigned int jobIndex, const std::string& sync);
    unsigned int addIPFSJob(const std::string& cid, const std::string& sync = "pin", const std::string& extra = "",
                            unsigned int maxSleep = 0, const std::string& callbackSymbol = "");
    std::future<std::string>
    addIPFSJobPromise(const std::string& cid, const std::string& sync = "", unsigned int maxTime = 0);
    // Returns a COPY of the stored callback (not a reference into the map) so it
    // stays valid to invoke even if another thread erases the entry. (audit M5)
    IPFSCallbackFunction getIPFSCallback(const std::string& callbackSymbol);
    unsigned int getIPFSJobCount();

    //DigiByte Domain table(these should only ever be called by DigiByteDomain.cpp
    void revokeDomain(const std::string& domain);
    void addDomain(const std::string& domain, const std::string& assetId);
    std::string getDomainAssetId(const std::string& domain, bool returnErrorIfRevoked = true);
    std::string getDomainAddress(const std::string& domain);
    bool isMasterDomainAssetId(const std::string& assetId) const;
    bool isActiveMasterDomainAssetId(const std::string& assetId) const;
    void setMasterDomainAssetId(const std::string& assetId);
    void setDomainCompromised();
    bool isDomainCompromised() const;

    //Unknown table
    void addUnknown(const std::string& txid, const Blob& data);
    void checkUnknown(const std::function<bool(const std::string&, const Blob&)>& callback); //callback takes (txid,data) and if known by callback returns true which removes from unknown table

    //Encrypted Keys table
    void addEncryptedKey(const std::string& address, const Blob& data);
    Blob getEncryptedKey(const std::string& address);

    //Permanent table
    void addToPermanent(unsigned int poolIndex, const std::string& cid);
    void removeFromPermanent(unsigned int poolIndex, const std::string& cid, bool unpin);
    void repinPermanent(unsigned int poolIndex);
    void unpinPermanent(unsigned int poolIndex);
    void addAssetToPool(unsigned int poolIndex, unsigned int assetIndex);
    void removeAssetFromPool(unsigned int poolIndex, const std::string& assetId, bool unpin);
    bool isAssetInPool(unsigned int poolIndex, unsigned int assetIndex);
    bool isAssetInPool(unsigned int assetIndex);
    std::vector<int> listPoolsAssetIsIn(unsigned int assetIndex);
    std::vector<std::string> getPSPFileList(unsigned int poolIndex);

    //stats table
    //warning a new stats table is created for every timeFrame.  It is not recommended to allow users direct access to this value
    void updateStats(unsigned int timeFrame = 86400);
    bool canGetAlgoStats();
    bool canGetAddressStats();
    std::vector<AlgoStats> getAlgoStats(unsigned int start = 0, unsigned int end = std::numeric_limits<unsigned int>::max(), unsigned int timeFrame = 86400);
    std::vector<AddressStats> getAddressStats(unsigned int start = 0, unsigned int end = std::numeric_limits<unsigned int>::max(), unsigned int timeFrame = 86400);

    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */
    // Base class for all Database errors. what() prefixes the stored message
    // with "Database Exception: ". The subclasses below specialize it per
    // failure kind (open, SQL command, insert/select/update/delete, statement
    // creation, reset, and requested-data-pruned).
    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "Database Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    class exceptionFailedToOpen : public exception {
    public:
        explicit exceptionFailedToOpen()
            : exception("Couldn't open or create the database") {}
    };

    class exceptionFailedSQLCommand : public exception {
    public:
        explicit exceptionFailedSQLCommand(const std::string& error = "SQL command failed")
            : exception(error) {}
    };


    class exceptionFailedToCreateTable : public exceptionFailedSQLCommand {
    public:
        explicit exceptionFailedToCreateTable(const std::string& error = "Table creation failed")
            : exceptionFailedSQLCommand(error) {}
    };

    class exceptionFailedInsert : public exceptionFailedSQLCommand {
    public:
        explicit exceptionFailedInsert(const std::string& error = "Insert failed")
            : exceptionFailedSQLCommand(error) {}
        explicit exceptionFailedInsert(int line)
            : exceptionFailedSQLCommand("Insert failed at Database.cpp:" + std::to_string(line)) {}
        exceptionFailedInsert(int line, const std::string& sqlError)
            : exceptionFailedSQLCommand("Insert failed at Database.cpp:" + std::to_string(line) + " (" + sqlError + ")") {}
    };

    class exceptionFailedSelect : public exceptionFailedSQLCommand {
    public:
        explicit exceptionFailedSelect(const std::string& error = "Select failed")
            : exceptionFailedSQLCommand(error) {}
    };

    class exceptionFailedUpdate : public exceptionFailedSQLCommand {
    public:
        explicit exceptionFailedUpdate(const std::string& error = "Update failed")
            : exceptionFailedSQLCommand(error) {}
    };

    class exceptionFailedDelete : public exceptionFailedSQLCommand {
    public:
        explicit exceptionFailedDelete(const std::string& error = "Delete failed")
            : exceptionFailedSQLCommand(error) {}
    };

    class exceptionCreatingStatement : public exceptionFailedSQLCommand {
    public:
        explicit exceptionCreatingStatement(const std::string& error = "Statement creation failed")
            : exceptionFailedSQLCommand(error) {}
    };

    class exceptionFailedReset : public exception {
    public:
        explicit exceptionFailedReset()
            : exception("Reset failed") {}
    };

    // Thrown when a caller requests history that has been pruned from the DB
    // (and cannot be recovered from the wallet/RPC).
    class exceptionDataPruned : public exception {
    public:
        explicit exceptionDataPruned()
            : exception("The requested data has been pruned") {}
    };
};

#endif //DIGIBYTECORE_DATABASECHAIN_H
