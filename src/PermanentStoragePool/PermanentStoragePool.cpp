//
// Created by mctrivia on 02/11/23.
//
//
// PermanentStoragePool.cpp - shared (non-virtual) behaviour of the PSP base class.
//
// Implements the pool logic common to every concrete Permanent Storage Pool:
// reading subscription/payout config, resolving a payout address (looking up
// or creating a wallet address when a label is given), repinning everything
// the pool should hold at startup, and the Database-backed bookkeeping for
// which assets belong to the pool.  Also implements the bad-asset / bad-file
// report flow (unpin locally, then optionally forward upstream) and the JSON
// summary used by the API.  The pure-virtual pool-specific hooks are supplied
// by the concrete subclasses in PermanentStoragePool/pools/.
//

#include "PermanentStoragePool.h"
#include "AppMain.h"
#include "Config.h"
#include "Log.h"
#include "static_block.hpp"


using namespace std;

/**
 * Assigns this pool its index and brings it fully online.
 *
 * Called once by PermanentStoragePoolList when the pool is added.  Stores the
 * pool index, then reads this pool's config keys (prefixed "psp<index>"):
 * "subscribe" (default true) and "autoremovebad" (default true).  If not
 * subscribed it returns early.  Otherwise it resolves the payout address from
 * "psp<index>payout" (default label "_psppayout"); when the value is a label
 * (leading '_') it looks up an existing wallet address for that label, or
 * creates a new one if none exists (logging CRITICAL and rethrowing on
 * failure).  Finally it repins everything this pool should hold, forwards the
 * config to the concrete pool via _setConfig(), and calls start().
 *
 * @param index  The pool's index within the pool list (also its Database key).
 * @param config Parsed node configuration.
 * @throws DigiByteException if a new payout address cannot be generated.
 * Side effects: may create a wallet address, repins IPFS files, starts the pool.
 */
void PermanentStoragePool::setPoolIndexAndInitialize(unsigned int index, const Config& config) {
    //save pool index
    _poolIndex = index;

    //save config data
    const string prefix = "psp" + to_string(index);
    _subscribed = config.getBool(prefix + "subscribe", defaultSubscribe());
    _autoRemoveBad = config.getBool(prefix + "autoremovebad", true);
    if (!_subscribed) return;

    //get payout address
    _payoutAddress = config.getString(prefix + "payout", "_psppayout");

    //handle condition payout address is a label
    if (!_payoutAddress.empty() && _payoutAddress[0] == '_') {
        DigiByteCore* dgb = AppMain::GetInstance()->getDigiByteCore();
        try {
            vector<string> addresses = dgb->getaddressesbylabel(_payoutAddress);
            _payoutAddress = addresses[0];
        } catch (const DigiByteException& e) {
            try {
                _payoutAddress = dgb->getnewaddress(_payoutAddress);
            } catch (const DigiByteException& e) {
                Log* log=Log::GetInstance();
                log->addMessage("Could not generate new PSP payout address",Log::CRITICAL);
                throw;
            }
        }
    }

    //at start make sure all data that should be pinned is pinned
    Database* db = AppMain::GetInstance()->getDatabase();
    db->repinPermanent(_poolIndex);

    //let the pool know the config data in case they have extra config info
    _setConfig(config);
    start();
}
unsigned int PermanentStoragePool::getPoolIndex() const {
    return _poolIndex;
}
/// Records in the Database that the given asset (all its files verified/pinned) belongs to this pool.
void PermanentStoragePool::markAssetAsPartOfPool(unsigned int assetIndex) {
    Database* db = AppMain::GetInstance()->getDatabase();
    db->addAssetToPool(_poolIndex, assetIndex);
}
/// Returns true if the Database has this asset marked as part of this pool.
bool PermanentStoragePool::isAssetPartOfPool(unsigned int assetIndex) const {
    Database* db = AppMain::GetInstance()->getDatabase();
    return db->isAssetInPool(_poolIndex, assetIndex);
}
/// Default bad-asset check; concrete pools override to consult their own blocklist. Base always returns false.
bool PermanentStoragePool::isAssetBad(const std::string& assetId) {
    return false;
}
/// Re-issues IPFS pins for every file this pool is responsible for (via Database::repinPermanent).
void PermanentStoragePool::repinAllFiles() const {
    Database* db = AppMain::GetInstance()->getDatabase();
    db->repinPermanent(_poolIndex);
}
bool PermanentStoragePool::subscribed() const {
    return _subscribed;
}
string PermanentStoragePool::getPayoutAddress() const {
    return _payoutAddress;
}

/**
 * Reports an asset as being in bad faith
 * @param assetId
 * @param internalOnly
 *   if true means the PSP called this function files will be unpinned if _autoRemoveBad is on
 *   if false means user called so files will be unpined and the PSP will receive warning that they may want to add to there list
 */
void PermanentStoragePool::reportAssetBad(const std::string& assetId, bool internalOnly) {
    //if calling as
    bool unpin = (!internalOnly || _autoRemoveBad);

    //remove the asset from the pool
    Database* db = AppMain::GetInstance()->getDatabase();
    db->removeAssetFromPool(_poolIndex, assetId, unpin);

    if (internalOnly) return;
    _reportAssetBad(assetId);
}

//override if you have a method of reporting bad assets
/// Default upstream "asset is bad" hook: pools with no reporting channel throw. Concrete pools override.
void PermanentStoragePool::_reportAssetBad(const std::string& assetId) {
    throw exceptionCouldntReport();
}

/**
 * Reports a file as being in bad faith
 * @param cid
 * @param internalOnly
 *   if true means the PSP called this function files will be unpinned if _autoRemoveBad is on
 *   if false means user called so files will be unpined and the PSP will receive warning that they may want to add to there list
 */
void PermanentStoragePool::reportFileBad(const string& cid, bool internalOnly) {
    //if calling as
    bool unpin = (!internalOnly || _autoRemoveBad);

    //remove the asset from the pool
    Database* db = AppMain::GetInstance()->getDatabase();
    db->removeFromPermanent(_poolIndex, cid, unpin);

    if (internalOnly) return;
    _reportAssetBad(cid);
}
/// Default upstream "file is bad" hook: pools with no reporting channel throw. Concrete pools override.
void PermanentStoragePool::_reportFileBad(const string& cid) {
    throw exceptionCouldntReport();
}
/// Default config hook (no-op); concrete pools override to read pool-specific config keys.
void PermanentStoragePool::_setConfig(const Config& config) {
}
/// Builds a JSON summary of the pool: name, description, url, and its list of pinned files.
Json::Value PermanentStoragePool::toJSON() {
    Json::Value results=Json::objectValue;
    results["name"]=getName();
    results["description"]=getDescription();
    results["url"]=getURL();
    results["files"]=Json::arrayValue;
    vector<string> files=getFiles();
    for (const auto& file: files) results["files"].append(file);
    return results;
}
std::vector<std::string> PermanentStoragePool::getFiles() {
    return AppMain::GetInstance()->getDatabase()->getPSPFileList(_poolIndex);
}
