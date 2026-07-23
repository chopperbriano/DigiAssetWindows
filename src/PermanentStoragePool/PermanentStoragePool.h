//
// Created by mctrivia on 02/11/23.
//
//
// PermanentStoragePool.h - abstract base class for a Permanent Storage Pool (PSP).
//
// A PSP is a scheme by which an asset creator pays (in DGB) to have their
// asset's IPFS files pinned forever by every node subscribed to that pool.
// This class is the common interface/behaviour shared by every concrete pool
// implementation (see PermanentStoragePool/pools/*, e.g. "local" and
// "mctrivia").  Concrete pools override the pure-virtual hooks to define how a
// transaction is recognised as belonging to the pool, how the cost is
// computed, how a transaction is modified to opt in, and how bad content is
// reported upstream.
//
// Roles this base class plays for the node:
//   - Chain Analyzer side: serialize/deserialize per-transaction metadata
//     processors and mark assets as belonging to the pool once their files
//     are pinned.
//   - Subscriber side: track subscription state + payout address, start/stop,
//     repin all files, and enumerate pinned files.
//   - Reporting side: unpin and (optionally) forward reports of bad assets or
//     bad files.
//   - Asset-creator side: enable a pool on a transaction and estimate cost.
//
// Pool bookkeeping (which assets/files belong to which pool, pin state) lives
// in the analyzer Database, keyed by this pool's _poolIndex.
//

#ifndef DIGIASSET_CORE_PERMANENTSTORAGEPOOL_H
#define DIGIASSET_CORE_PERMANENTSTORAGEPOOL_H



#include "Config.h"
#include "DigiByteTransaction.h"
#include "PermanentStoragePoolMetaProcessor.h"
#include <string>


/**
 * Abstract base class for every Permanent Storage Pool implementation.
 *
 * Holds shared subscription/config state and non-virtual helpers that talk to
 * the Database, while leaving pool-specific policy (recognition, cost, opt-in,
 * upstream reporting, metadata) to pure-virtual methods concrete pools must
 * implement.  Instances are created and owned by PermanentStoragePoolList,
 * which assigns each pool its _poolIndex.
 */
class PermanentStoragePool {
private:
    bool _subscribed = true;
    std::string _payoutAddress;
    bool _autoRemoveBad = true;

protected:
    unsigned int _poolIndex;
    virtual void _reportAssetBad(const std::string& assetId);
    virtual void _reportFileBad(const std::string& cid);
    virtual void _setConfig(const Config& config);
    // Default subscription state when psp<index>subscribe is absent from config.
    // true for the built-in pools (local, mctrivia) so existing nodes keep
    // working after an update; NEW pools override this to false so they are
    // strictly opt-in and never resolve a payout address for a node whose
    // config predates them.
    virtual bool defaultSubscribe() const { return true; }



public:
    virtual ~PermanentStoragePool() = default;

    //called by PoolList
    void setPoolIndexAndInitialize(unsigned int index, const Config& config);

    //called by Chain Analyzer
    virtual std::string serializeMetaProcessor(const DigiByteTransaction& tx) = 0;                                              //if tx is part of PSP returns serialized data for processing metadata if not returns empty
    virtual std::unique_ptr<PermanentStoragePoolMetaProcessor> deserializeMetaProcessor(const std::string& serializedData) = 0; //create object for processing what should be pinned
    void markAssetAsPartOfPool(unsigned int assetIndex);                                                                        //called after verifying all files are part of PSP

    //called by Node Operators that subscribe to PSP(actually usually called automatically when subscribing and unsubscribing or at start if subscribed)
    virtual void start() = 0;
    virtual void stop() = 0;

    //called by api
    bool subscribed() const;                               //returns if currently subscribe to the pool or not
    std::string getPayoutAddress() const;                  //if subscribed returns payout address you subscribed with(returns error if not subscribed)
    bool isAssetPartOfPool(unsigned int assetIndex) const; //returns true if all files are part of PSP
    void repinAllFiles() const;
    unsigned int getPoolIndex() const;
    virtual bool isAssetBad(const std::string& assetId);                        //if not overridden always returns false
    void reportAssetBad(const std::string& assetId, bool internalOnly = false); //if PSP calling set internalOnly so it doesnt call psp
    void reportFileBad(const std::string& cid, bool internalOnly = false);      //if PSP calling set internalOnly so it doesnt call psp
    std::vector<std::string> getFiles();
    Json::Value toJSON();

    //called by asset creator
    virtual void enable(DigiByteTransaction& tx) = 0;            //makes changes to tx to enable psp on that transaction(must be called last before publishing.  Code must allow for 240 block delay in publishing)
    virtual uint64_t getCost(const DigiByteTransaction& tx) = 0; //estimates the cost of using this psp and returns in DGB sats(may not be exact since exchange rates may change)
    virtual std::string getName() = 0;                           //gets the name of the PSP
    virtual std::string getDescription() = 0;                    //gets the description
    virtual std::string getURL() = 0;                            //gets the PSP's website

    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */

    /**
     * Base exception type for all PSP errors; what() prefixes the stored
     * message with "Permanent Storage Pool Exception: ".
     */
    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "Permanent Storage Pool Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    /// Thrown by enable() when the given transaction cannot be made to opt into the pool.
    class exceptionCantEnablePSP : public exception {
    public:
        explicit exceptionCantEnablePSP()
            : exception("Tried to enable PSP on a transaction that wasn't possible to enable") {}
    };

    /// Thrown when a pool fails to load/initialize.
    class exceptionCantLoadPSP : public exception {
    public:
        explicit exceptionCantLoadPSP()
            : exception("Couldn't load the PSP") {}
    };

    /// Thrown by the default report hooks when a pool has no way to forward a bad-content report upstream.
    class exceptionCouldntReport : public exception {
    public:
        explicit exceptionCouldntReport()
            : exception("Couldn't report to PSP") {}
    };
};



#endif //DIGIASSET_CORE_PERMANENTSTORAGEPOOL_H
