//
// Created by mctrivia on 02/02/23.
//
// ChainAnalyzer implementation — the node's chain-indexing worker.
//
// Contains the sync loop (phaseSync), the fork-recovery logic (phaseRewind),
// the pruning logic (phasePrune), per-transaction processing (processTX), and
// the config load/save that persists prune/storage preferences to config.cfg.
// Runs on the Threaded worker thread; on any thrown exception mainFunction()
// is re-entered so a bad block is retried with backoff before giving up.
//

#include "ChainAnalyzer.h"
#include "AppMain.h"
#include "BitIO.h"
#include "Config.h"
#include "Database.h"
#include "DigiAsset.h"
#include "DigiByteCore.h"
#include "DigiByteTransaction.h"
#include "KYC.h"
#include "Log.h"
#include "PermanentStoragePool/PermanentStoragePoolList.h"
#include "utils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <thread>

using namespace std;


std::map<std::string, int> ChainAnalyzer::_pinAssetExtraMimeTypes;

/*
 ██████╗ ██████╗ ███╗   ██╗███████╗████████╗██████╗ ██╗   ██╗ ██████╗████████╗ ██████╗ ██████╗
██╔════╝██╔═══██╗████╗  ██║██╔════╝╚══██╔══╝██╔══██╗██║   ██║██╔════╝╚══██╔══╝██╔═══██╗██╔══██╗
██║     ██║   ██║██╔██╗ ██║███████╗   ██║   ██████╔╝██║   ██║██║        ██║   ██║   ██║██████╔╝
██║     ██║   ██║██║╚██╗██║╚════██║   ██║   ██╔══██╗██║   ██║██║        ██║   ██║   ██║██╔══██╗
╚██████╗╚██████╔╝██║ ╚████║███████║   ██║   ██║  ██║╚██████╔╝╚██████╗   ██║   ╚██████╔╝██║  ██║
 ╚═════╝ ╚═════╝ ╚═╝  ╚═══╝╚══════╝   ╚═╝   ╚═╝  ╚═╝ ╚═════╝  ╚═════╝   ╚═╝    ╚═════╝ ╚═╝  ╚═╝
 */

ChainAnalyzer::ChainAnalyzer() {
    //reset config variables
    resetConfig();
}

ChainAnalyzer::~ChainAnalyzer() {
    stop();
}

/*
 ██████╗ ██████╗ ███╗   ██╗███████╗██╗ ██████╗
██╔════╝██╔═══██╗████╗  ██║██╔════╝██║██╔════╝
██║     ██║   ██║██╔██╗ ██║█████╗  ██║██║  ███╗
██║     ██║   ██║██║╚██╗██║██╔══╝  ██║██║   ██║
╚██████╗╚██████╔╝██║ ╚████║██║     ██║╚██████╔╝
 ╚═════╝ ╚═════╝ ╚═╝  ╚═══╝╚═╝     ╚═╝ ╚═════╝
 */

/**
 * Stops the worker (if running) and restores every state and config variable to
 * its built-in default. Called by the constructor and whenever the config file
 * changes. Note the meta-data pin settings are static and not reset here.
 */
void ChainAnalyzer::resetConfig() {
    stop();

    //default state values
    _height = 1;
    _nextHash = "";

    //default config values(chain data)
    _pruneAge = 5760; //number of blocks to keep for roll back protection(-1 don't prune, default is 1 day)
    _pruneInterval = (int) ceil(_pruneAge / PRUNE_INTERVAL_DIVISOR / 100) * 100;
    _pruneExchangeHistory = true;
    _pruneUTXOHistory = true;
    _pruneVoteHistory = true;
    _verifyDatabaseWrite = true;
    _showAllBlockSyncTime = false;
}

/**
 * Changes what config file we should use
 * @param fileName
 */
void ChainAnalyzer::setFileName(const std::string& fileName) {
    //make change
    _configFileName = fileName;

    //make sure chain analyzer is shut down and reset
    resetConfig();

    //if file exists load it
    try {
        loadConfig();
    } catch (const Config::exceptionConfigFileMissing& e) {
        //no config file so just ignore
    }
}

/**
 * Reads the config file named by _configFileName and applies every persisted
 * chain-data setting (prune ages/flags, storage flags, verify/verbose flags)
 * into this object via the setters. Throws Config::exceptionConfigFileMissing
 * if the file does not exist (caught by setFileName).
 */
void ChainAnalyzer::loadConfig() {
    Config config = Config(_configFileName);

    //load values in to class(chain data)
    setPruneAge(config.getInteger("pruneage", 5760)); //-1 for don't prune, default daily
    setPruneExchangeHistory(config.getBool("pruneexchangehistory", true));
    setPruneUTXOHistory(config.getBool("pruneutxohistory", true));
    setPruneVoteHistory(config.getBool("prunevotehistory", true));
    setStoreNonAssetUTXO(config.getBool("storenonassetutxo", false));
    _verifyDatabaseWrite = config.getBool("verifydatabasewrite", true);
    _showAllBlockSyncTime = config.getBool("showallblocksynctimes", false);
}

/**
 * This function is used for testing purposes.  It allows creating the object without starting but still having realistic values
 * @param databaseHeight
 * @param syncLevel
 */
void ChainAnalyzer::loadFake(unsigned int databaseHeight, int syncLevel) {
    _height = databaseHeight;
    _state = syncLevel;
}

/**
 * Writes the current prune/storage settings plus the static extra-pin MIME-type
 * map back to the config file, preserving any comments/ordering already present
 * (see Config::write). Overwrites _configFileName in place.
 */
void ChainAnalyzer::saveConfig() {
    Config config = Config(_configFileName);
    config.setInteger("pruneage", _pruneAge);
    config.setBool("pruneexchangehistory", _pruneExchangeHistory);
    config.setBool("pruneutxohistory", _pruneUTXOHistory);
    config.setBool("prunevotehistory", _pruneVoteHistory);
    config.setBool("storenonassetutxo", _storeNonAssetUTXOs);
    config.setIntegerMap("pinassetextra", _pinAssetExtraMimeTypes);
    config.write();
}


bool ChainAnalyzer::shouldPruneExchangeHistory() const {
    return _pruneExchangeHistory;
}

/**
 * Enables/disables pruning of exchange-rate history. Refuses (throws
 * exceptionAlreadyPruned) to turn pruning OFF if the DB has already pruned this
 * data, because the removed history cannot be recovered without a full resync.
 */
void ChainAnalyzer::setPruneExchangeHistory(bool shouldPrune) {
    Database* db = AppMain::GetInstance()->getDatabase();
    if (!shouldPrune && (db->getBeenPrunedExchangeHistory() >= 0)) throw exceptionAlreadyPruned();
    _pruneExchangeHistory = shouldPrune;
}

bool ChainAnalyzer::shouldPruneUTXOHistory() const {
    return _pruneUTXOHistory;
}

/**
 * Enables/disables pruning of spent-UTXO history. Throws exceptionAlreadyPruned
 * if asked to disable pruning after the DB has already pruned this data.
 */
void ChainAnalyzer::setPruneUTXOHistory(bool shouldPrune) {
    Database* db = AppMain::GetInstance()->getDatabase();
    if (!shouldPrune && (db->getBeenPrunedUTXOHistory() >= 0)) throw exceptionAlreadyPruned();
    _pruneUTXOHistory = shouldPrune;
}

bool ChainAnalyzer::shouldPruneVoteHistory() const {
    return _pruneVoteHistory;
}

/**
 * Enables/disables pruning of vote history. Throws exceptionAlreadyPruned if
 * asked to disable pruning after the DB has already pruned this data.
 */
void ChainAnalyzer::setPruneVoteHistory(bool shouldPrune) {
    Database* db = AppMain::GetInstance()->getDatabase();
    if (!shouldPrune && (db->getBeenPrunedVoteHistory() >= 0)) throw exceptionAlreadyPruned();
    _pruneVoteHistory = shouldPrune;
}

bool ChainAnalyzer::shouldStoreNonAssetUTXO() const {
    return _storeNonAssetUTXOs;
}

/**
 * Sets whether UTXOs that carry no DigiAsset data should still be stored. Throws
 * exceptionAlreadyPruned if asked to START storing them after the DB has already
 * been told to discard them (they'd be missing for past blocks).
 */
void ChainAnalyzer::setStoreNonAssetUTXO(bool shouldStore) {
    Database* db = AppMain::GetInstance()->getDatabase();
    if (shouldStore && (db->getBeenPrunedNonAssetUTXOHistory())) throw exceptionAlreadyPruned();
    _storeNonAssetUTXOs = shouldStore;
}


/**
 * returns 0 if we should not prune right now otherwise returns height we can prune up to
 * @param height
 * @return
 */
unsigned int ChainAnalyzer::pruneMax(unsigned int height) {
    if (_pruneAge < 0) return 0;                //no pruning
    if (height % _pruneInterval != 0) return 0; //not time to prune
    if (height <= (unsigned) _pruneAge) return 0; //below the prune window - unsigned, so the old (height-_pruneAge<0) was dead code (audit low)
    return height - (unsigned) _pruneAge;
}

/**
 * Sets how many recent blocks of history to retain (-1 disables pruning) and
 * recomputes _pruneInterval — how often pruning runs — as a multiple of 100
 * blocks derived from the age and PRUNE_INTERVAL_DIVISOR.
 */
void ChainAnalyzer::setPruneAge(int age) {
    _pruneAge = age;
    _pruneInterval = (int) ceil(1.0 * _pruneAge / PRUNE_INTERVAL_DIVISOR / 100) *
                     100; //make sure prune interval is multiple of 100
}


/*
██╗      ██████╗  ██████╗ ██████╗
██║     ██╔═══██╗██╔═══██╗██╔══██╗
██║     ██║   ██║██║   ██║██████╔╝
██║     ██║   ██║██║   ██║██╔═══╝
███████╗╚██████╔╝╚██████╔╝██║
╚══════╝ ╚═════╝  ╚═════╝ ╚═╝
 */

/**
 * Threaded startup hook, run once when the worker thread launches. Marks state
 * INITIALIZING, reads the last stored block height from the DB, clears any
 * partially-processed block above it (crash recovery), and records whether
 * non-asset UTXOs are being stored so the DB behaves consistently.
 */
void ChainAnalyzer::startupFunction() {
    Log* log = Log::GetInstance();

    //mark as initializing
    _state = INITIALIZING;
    AppMain* main = AppMain::GetInstance();
    Database* db = main->getDatabase();
    DigiByteCore* dgb = main->getDigiByteCore();

    //make sure everything is set up.
    // Always use the fast/relaxed-durability pragmas for the genesis->tip
    // catch-up: chain.db is fully re-derivable from the blockchain, so a crash
    // mid-sync just means re-syncing - and this is the single biggest
    // initial-sync speedup (avoids an fsync per block + gives a 256MB cache).
    // If the operator asked for durable writes (verifydatabasewrite=true, the
    // default), they're restored once we reach the tip in phaseSync().
    db->disableWriteVerification();
    _writeVerificationRestored = false;

    //find block we left off at
    _height = db->getBlockHeight();
    _nextHash = dgb->getBlockHash(_height);

    //clear the block we left off on just in case it was partially processed
    log->addMessage("Repairing database from shutdown");
    db->clearBlocksAboveHeight(_height);
    log->addMessage("Repair complete");

    //make sure database knows if we want to store non asset utxos
    if (!shouldStoreNonAssetUTXO()) {
        //mark as has been pruned if we aren't keeping and database will not store them
        db->setBeenPrunedNonAssetUTXOHistory(true);
    }
}

/**
 * Threaded main hook — the body of the sync loop. On the first call it runs
 * straight through; on any later call (meaning a previous run threw) it first
 * re-reads height from the DB, rolls back the failed block, and applies an
 * error-count backoff: sleep after 3 consecutive failures, and after 10 give up
 * (set STOPPED and idle until stop is requested). On success it runs phaseRewind
 * then phaseSync and resets the error count; on exception it logs and re-throws
 * so the Threaded framework re-enters this function to retry.
 */
void ChainAnalyzer::mainFunction() {
    Log* log = Log::GetInstance();

    // On the first call after startupFunction(), state is already correct.
    // On subsequent calls (after an exception), re-read from DB to recover.
    if (_hasRunOnce) {
        _errorCount++;
        AppMain* main = AppMain::GetInstance();
        Database* db = main->getDatabase();
        DigiByteCore* dgb = main->getDigiByteCore();
        _height = db->getBlockHeight();
        _nextHash = dgb->getBlockHash(_height);
        db->clearBlocksAboveHeight(_height);
        log->addMessage("Recovered from error at block " + std::to_string(_height) +
            " (attempt " + std::to_string(_errorCount) + ")", Log::WARNING);

        // If we keep failing at the same block, wait before retrying
        if (_errorCount >= 3) {
            log->addMessage("Repeated failures - waiting 10 seconds before retry", Log::WARNING);
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        if (_errorCount >= 10) {
            log->addMessage("Too many failures at block " + std::to_string(_height + 1) +
                " - stopping sync. Restart to try again.", Log::CRITICAL);
            _state = STOPPED;
            // Sleep forever until stop is requested — prevents Threaded from retrying
            while (!stopRequested()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            return;
        }
    }
    _hasRunOnce = true;

    try {
        phaseRewind();
        phaseSync();
        _errorCount = 0; // reset on success
    } catch (const std::exception& e) {
        log->addMessage("Sync error at block " + std::to_string(_height) + ": " + e.what(), Log::CRITICAL);
        throw; // re-throw so Threaded framework catches it and calls mainFunction again
    }
}

void ChainAnalyzer::shutdownFunction() {
    _state = STOPPED;
}

/*
██████╗ ██╗  ██╗ █████╗ ███████╗███████╗███████╗
██╔══██╗██║  ██║██╔══██╗██╔════╝██╔════╝██╔════╝
██████╔╝███████║███████║███████╗█████╗  ███████╗
██╔═══╝ ██╔══██║██╔══██║╚════██║██╔══╝  ╚════██║
██║     ██║  ██║██║  ██║███████║███████╗███████║
╚═╝     ╚═╝  ╚═╝╚═╝  ╚═╝╚══════╝╚══════╝╚══════╝
 */

/**
 * Fork/reorg recovery. Compares the DigiByte Core block hash at the current
 * height against the hash we expect (_nextHash). If they differ the local chain
 * reorganized, so it walks backwards one block at a time until the stored and
 * live hashes agree, then deletes all DB data at and above that point. If the
 * rewind reaches a height that was already pruned, it calls restart() to resync
 * from block 1. No-op when no fork is detected.
 */
void ChainAnalyzer::phaseRewind() {
    Log* log = Log::GetInstance();

    AppMain* main = AppMain::GetInstance();
    Database* db = main->getDatabase();
    DigiByteCore* dgb = main->getDigiByteCore();

    //check if we need to rewind (blockchain fork detected)
    string hash = dgb->getBlockHash(_height);
    if (hash != _nextHash) {
        log->addMessage("Fork detected at block " + std::to_string(_height) + " - rewinding", Log::WARNING);
        _state = ChainAnalyzer::REWINDING;

        //rewind until correct
        unsigned int originalHeight = _height;
        while (hash != _nextHash) {
            _height--;
            hash = dgb->getBlockHash(_height);
            try {
                _nextHash = db->getBlockHash(_height);
            } catch (const Database::exceptionDataPruned& e) {
                //we rolled back to point that has been pruned so restart chain analyser
                log->addMessage("Rewound blocks past prune point.  Need to restart sync", Log::WARNING);
                restart();
                return;
            }
        }
        log->addMessage("Rewinding " + to_string(originalHeight - _height) + " blocks");

        //delete all data above & including _height
        db->clearBlocksAboveHeight(_height);
        log->addMessage("Rewinding Phase Ended");
    }
}

/**
 * The main indexing loop. Starting at the current height it fetches each block
 * from DigiByte Core (using verbose/verbosity-2 RPC to pull a block plus all its
 * transactions in one call during bulk catch-up), updates the sync state to the
 * number of blocks behind, and — for blocks at/after the DigiAssets activation
 * height (or when storing non-asset UTXOs) — processes every transaction inside
 * a DB transaction. It logs per-block or per-100-block timing/ETA, invalidates
 * the RPC cache on new tip blocks, prunes when due, and once caught up polls
 * every 500ms for the next block. Batches header inserts for pre-asset blocks.
 * Returns when the chain tip changes under it (to re-enter rewind) or when stop
 * is requested.
 */
void ChainAnalyzer::phaseSync() {
    Log* log = Log::GetInstance();

    AppMain* main = AppMain::GetInstance();
    Database* db = main->getDatabase();
    DigiByteCore* dgb = main->getDigiByteCore();

    //start syncing
    string hash = dgb->getBlockHash(_height);
    bool fastMode = false;
    chrono::steady_clock::time_point beginTime;
    chrono::steady_clock::time_point beginTotalTime;
    long totalProcessed = 0;
    int insertBatch = 0;
    stringstream ss;

    bool needsAssetInit = (shouldStoreNonAssetUTXO() || (_height >= 8432316));
    blockinfo_t blockData = needsAssetInit ? dgb->getBlockVerbose(hash) : dgb->getBlock(hash);

    while ((hash == _nextHash) && !stopRequested()) {
        if (totalProcessed == 0) {
            beginTotalTime = chrono::steady_clock::now();
        }

        //determine sync mode
        _state = 0 - blockData.confirmations;
        bool bulkSync = (_state < -110);
        bool needsAssetProcessing = (shouldStoreNonAssetUTXO() || (_height >= 8432316));
        if (!_showAllBlockSyncTime && (_height % 100 == 0)) fastMode = bulkSync;

        // Pipeline disabled for stability — direct RPC only

        //show processing block
        if (fastMode) {
            if (_height % 100 == 0) {
                ss << "processed blocks: " << setw(9) << _height << " to " << setw(9) << (_height + 99);
                beginTime = chrono::steady_clock::now();
            }
        } else {
            ss << "processed block: " << setw(9) << _height;
            beginTime = chrono::steady_clock::now();
        }
        if (!fastMode) ss << "(" << setw(8) << (_state + 1) << ") ";

        //process each tx in block
        if (needsAssetProcessing) {
            db->startTransaction();
            for (string& tx: blockData.tx)
                processTX(tx, blockData.height);
            db->endTransaction();
        }

        //show run time stats
        totalProcessed++;
        // Log profiling every 500 blocks
        if (fastMode && (_height % 500 == 499) && _processTransactionRunCount > 0) {
            log->addMessage("PERF: " + to_string(_processTransactionRunCount) + " txs"
                + " rpc=" + to_string(_processTransactionRunTime/1000) + "ms"
                + " db=" + to_string(_saveTransactionRunTime/1000) + "ms"
                + " tx/blk=" + to_string(_processTransactionRunCount/500), Log::DEBUG);
            _processTransactionRunTime = 0; _processTransactionRunCount = 0;
            _saveTransactionRunTime = 0; _saveTransactionRunCount = 0;
        }
        if (fastMode) {
            if (_height % 100 == 99) {
                chrono::steady_clock::time_point endTime = chrono::steady_clock::now();
                unsigned long msRemaining = blockData.confirmations * chrono::duration_cast<chrono::milliseconds>(endTime - beginTotalTime).count() / totalProcessed;
                ss << " in " << setw(6) << chrono::duration_cast<chrono::milliseconds>(endTime - beginTime).count() / 100 << " ms per block - ";
                const unsigned long msPerMinute = 60000, msPerHour = 3600000, msPerDay = 86400000;
                if (msRemaining >= msPerDay * 2) {
                    ss << std::fixed << std::setprecision(1) << msRemaining / (double)msPerDay << " days left to sync";
                } else if (msRemaining >= msPerHour * 2) {
                    ss << std::fixed << std::setprecision(1) << msRemaining / (double)msPerHour << " hours left to sync";
                } else {
                    ss << std::fixed << std::setprecision(1) << msRemaining / (double)msPerMinute << " minutes left to sync";
                }
                log->addMessage(ss.str(), Log::DEBUG);
                ss.str(""); ss.clear();
            }
        } else {
            chrono::steady_clock::time_point endTime = chrono::steady_clock::now();
            ss << " in " << setw(6) << chrono::duration_cast<chrono::milliseconds>(endTime - beginTime).count() << " ms per block";
            log->addMessage(ss.str(), Log::DEBUG);
            ss.str(""); ss.clear();
        }

        //clear invalid RPC cached (skip during bulk sync — nobody is querying)
        if (!bulkSync) {
            AppMain::GetInstance()->getRpcCache()->newBlockAdded();
        }

        //prune database
        phasePrune();

        //if fully synced pause until new block
        while (blockData.nextblockhash.empty()) {
            db->executePerformanceIndex(_state);
            // Reached the tip. If the operator wants durable writes, restore them
            // now that the fast catch-up is done - but only after the deferred
            // performance indexes have all been built (they build faster under
            // the relaxed pragmas). One-time per run.
            if (_verifyDatabaseWrite && !_writeVerificationRestored &&
                !db->performanceIndexesPending()) {
                db->enableWriteVerification();
                _writeVerificationRestored = true;
                Log::GetInstance()->addMessage(
                    "Sync complete - durable write mode restored (verifydatabasewrite=1)", Log::INFO);
            }
            _state = SYNCED;
            totalProcessed = 0;
            chrono::milliseconds dura(500);
            this_thread::sleep_for(dura);
            string currentHash = dgb->getBlockHash(_height);
            if (hash != currentHash) { _state = REWINDING; return; }
            blockData = dgb->getBlock(hash);
        }

        //advance to next block
        _nextHash = blockData.nextblockhash;
        _height++;

        //get next block — use verbosity 2 during bulk sync to get all TX data in one call
        if (bulkSync && (_height % 100 != 0)) {
            hash = _nextHash;
        } else {
            hash = dgb->getBlockHash(_height);
        }
        if (needsAssetProcessing && bulkSync) {
            blockData = dgb->getBlockVerbose(hash); // 1 RPC call = block + all TXs
        } else {
            blockData = dgb->getBlock(hash);
        }

        //save block header to database (batched for pre-asset blocks)
        if (!needsAssetProcessing && insertBatch == 0) {
            db->startTransaction();
        }
        db->insertBlock(blockData.height, blockData.hash, blockData.time, blockData.algo, blockData.difficulty);
        if (!needsAssetProcessing) {
            insertBatch++;
            if (insertBatch >= 100) {
                db->endTransaction();
                insertBatch = 0;
            }
        }
    }

    //cleanup
    if (insertBatch > 0) db->endTransaction();
}

/**
 * Prunes historical data when the current height is a pruning boundary. Asks
 * pruneMax() for the height to prune up to (0 = not now / disabled) and, based
 * on the enabled prune flags, tells the DB to drop exchange, UTXO, and vote
 * history below that height. Exchange pruning keeps an extra leniency window so
 * recent exchange-rate lookups still work.
 */
void ChainAnalyzer::phasePrune() {

    //check if time to prune
    unsigned int pruneHeight = pruneMax(_height);
    if (pruneHeight == 0) return;

    //prune the data
    Database* db = AppMain::GetInstance()->getDatabase();
    if (shouldPruneExchangeHistory()) db->pruneExchange(min(pruneHeight, _height - DigiAsset::EXCHANGE_RATE_LENIENCY));
    if (shouldPruneUTXOHistory()) db->pruneUTXO(pruneHeight);
    if (shouldPruneVoteHistory()) db->pruneVote(pruneHeight);
}

/**
 * Wipes the entire database and resets the analyzer to block 1 so syncing starts
 * over from genesis. Used when a rewind lands past the prune point.
 */
void ChainAnalyzer::restart() {
    Database* db = AppMain::GetInstance()->getDatabase();
    db->reset();
    _height = 1;
    _nextHash = DIGIBYTE_BLOCK1_HASH;
}

/*
██████╗ ██████╗  ██████╗  ██████╗███████╗███████╗███████╗
██╔══██╗██╔══██╗██╔═══██╗██╔════╝██╔════╝██╔════╝██╔════╝
██████╔╝██████╔╝██║   ██║██║     █████╗  ███████╗███████╗
██╔═══╝ ██╔══██╗██║   ██║██║     ██╔══╝  ╚════██║╚════██║
██║     ██║  ██║╚██████╔╝╚██████╗███████╗███████║███████║
╚═╝     ╚═╝  ╚═╝ ╚═════╝  ╚═════╝╚══════╝╚══════╝╚══════╝
 */

/**
 * Processes a single transaction by id at the given height: parses it (as a
 * DigiByteTransaction, which decodes any DigiAsset payload and, when configured,
 * drops non-asset UTXOs), writes it to the database, and — outside of bulk sync
 * — invalidates the RPC cache for every input/output address it touched. Also
 * accumulates the profiling timers for parse, save, and cache-clear.
 */
void ChainAnalyzer::processTX(const string& txid, unsigned int height) {
    //get raw transaction
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    DigiByteTransaction tx(txid, height, !_storeNonAssetUTXOs);
    auto duration = std::chrono::steady_clock::now() - startTime;
    _processTransactionRunTime += std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    _processTransactionRunCount++;

    //add transaction to database
    startTime = std::chrono::steady_clock::now();
    tx.addToDatabase();
    duration = std::chrono::steady_clock::now() - startTime;
    _saveTransactionRunTime += std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    _saveTransactionRunCount++;

    //invalidate rpc caches for changed addresses (skip during bulk sync)
    if (_state >= -110) {
        startTime = std::chrono::steady_clock::now();
        vector<string> addresses;
        size_t inputCount = tx.getInputCount();
        for (size_t i = 0; i < inputCount; i++) {
            addresses.emplace_back(tx.getInput(i).address);
        }
        size_t outputCount = tx.getOutputCount();
        for (size_t i = 0; i < outputCount; i++) {
            addresses.emplace_back(tx.getOutput(i).address);
        }
        std::sort(addresses.begin(), addresses.end());
        auto last = std::unique(addresses.begin(), addresses.end());
        addresses.erase(last, addresses.end());
        RPC::Cache* cache = AppMain::GetInstance()->getRpcCache();
        for (auto address: addresses) {
            cache->addressChanged(address);
        }
        duration = std::chrono::steady_clock::now() - startTime;
        _clearAddressCacheRunTime += std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        _clearAddressCacheRunCount++;
    }
}


/**
 * Gets the current sync state
 */
int ChainAnalyzer::getSync() const {
    return _state;
}
unsigned int ChainAnalyzer::getSyncHeight() const {
    return _height;
}
