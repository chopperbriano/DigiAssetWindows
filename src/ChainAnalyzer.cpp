//
// Created by mctrivia on 02/02/23.
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

void ChainAnalyzer::setPruneExchangeHistory(bool shouldPrune) {
    Database* db = AppMain::GetInstance()->getDatabase();
    if (!shouldPrune && (db->getBeenPrunedExchangeHistory() >= 0)) throw exceptionAlreadyPruned();
    _pruneExchangeHistory = shouldPrune;
}

bool ChainAnalyzer::shouldPruneUTXOHistory() const {
    return _pruneUTXOHistory;
}

void ChainAnalyzer::setPruneUTXOHistory(bool shouldPrune) {
    Database* db = AppMain::GetInstance()->getDatabase();
    if (!shouldPrune && (db->getBeenPrunedUTXOHistory() >= 0)) throw exceptionAlreadyPruned();
    _pruneUTXOHistory = shouldPrune;
}

bool ChainAnalyzer::shouldPruneVoteHistory() const {
    return _pruneVoteHistory;
}

void ChainAnalyzer::setPruneVoteHistory(bool shouldPrune) {
    Database* db = AppMain::GetInstance()->getDatabase();
    if (!shouldPrune && (db->getBeenPrunedVoteHistory() >= 0)) throw exceptionAlreadyPruned();
    _pruneVoteHistory = shouldPrune;
}

bool ChainAnalyzer::shouldStoreNonAssetUTXO() const {
    return _storeNonAssetUTXOs;
}

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
    if (height - _pruneAge < 0) return 0;
    return height - _pruneAge;
}

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

void ChainAnalyzer::startupFunction() {
    Log* log = Log::GetInstance();

    //mark as initializing
    _state = INITIALIZING;
    AppMain* main = AppMain::GetInstance();
    Database* db = main->getDatabase();
    DigiByteCore* dgb = main->getDigiByteCore();

    //make sure everything is set up
    if (!_verifyDatabaseWrite) db->disableWriteVerification();

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
