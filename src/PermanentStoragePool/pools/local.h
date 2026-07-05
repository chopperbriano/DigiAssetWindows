//
// Created by mctrivia on 12/11/23.
//
//
// local.h - the "Local Storage" Permanent Storage Pool implementation.
//
// This pool has no external server and no cost: it simply records, in a small
// local SQLite database (local.db), which asset CIDs the node operator has
// opted into pinning, plus a list of asset ids flagged as bad. Because there is
// no remote pool, metadata is only preserved for as long as this node keeps
// running - hence the strongly-discouraged warning in getDescription(). Used by
// the node (DigiAssetWindows.exe) as one selectable PSP among several.
//

#ifndef DIGIASSET_CORE_LOCAL_H
#define DIGIASSET_CORE_LOCAL_H


#define PSP_LOCAL_DB_FILENAME "local.db"

#include "PermanentStoragePool/PermanentStoragePool.h"


/**
 * "Local Storage" pool. Overrides the PSP interface to store opt-in CIDs and
 * bad-asset ids in a lazily-opened SQLite database (local.db) instead of
 * talking to any pool server. All prepared statements and the db handle are
 * created on first use via loadDB(). enable() adds a transaction's CID to the
 * local pin table rather than modifying the on-chain transaction; getCost()
 * is always 0.
 */
class local : public PermanentStoragePool {
private:
    sqlite3* _db = nullptr;
    sqlite3_stmt* _stmtCheckIfPartOfPool = nullptr;
    sqlite3_stmt* _stmtCheckIfBad = nullptr;
    sqlite3_stmt* _stmtEnableInPool = nullptr;
    sqlite3_stmt* _stmtMarkBad = nullptr;
    sqlite3_stmt* _stmtDiableFromPool = nullptr;

    bool localExists() const;   //true when local.db is absent (stat fails); used as the first-run flag
    void loadDB();              //lazily opens/creates local.db and prepares statements; no-op if already loaded
    void buildTables();         //creates the "pin" and "bad" tables on first run
    void initializeDBValues();  //prepares the reusable SQL statements against the open db

protected:
    void _reportAssetBad(const std::string& assetId) override;
    void _reportFileBad(const std::string& cid) override;

public:
    local() = default;

    //called by Node Operators that subscribe to PSP
    std::string serializeMetaProcessor(const DigiByteTransaction& tx) override;                                              //if tx is part of PSP returns serialized data for processing metadata if not returns empty
    std::unique_ptr<PermanentStoragePoolMetaProcessor> deserializeMetaProcessor(const std::string& serializedData) override; //create object for processing what should be pinned
    void start() override;
    void stop() override;

    //called by API
    bool isAssetBad(const std::string& assetId) override;

    //called by asset creator
    void enable(DigiByteTransaction& tx) override;            //makes changes to tx to enable psp on that transaction(must be called last before publishing)
    uint64_t getCost(const DigiByteTransaction& tx) override; //estimates the cost of using this psp and returns in DGB sats(may not be exact since exchange rates may change)
    std::string getName() override;                           //gets the name of the PSP
    std::string getDescription() override;                    //gets the description
    std::string getURL() override;                            //gets the PSP's website
};

/**
 * Metadata processor for the local pool. The serialized payload carries no real
 * information (it is always "1"), so this processor unconditionally pins every
 * file of an asset that was found in the local pin table.
 */
class localMetaProcessor : public PermanentStoragePoolMetaProcessor {
public:
    localMetaProcessor(const std::string& serializedData, unsigned int poolIndex);
    bool _shouldPinFile(const std::string& name, const std::string& mimeType, const std::string& cid) override; //called for each file included in asset returns if file should be pinned
};



#endif //DIGIASSET_CORE_LOCAL_H
