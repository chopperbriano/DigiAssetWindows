//
// Created by mctrivia on 04/11/23.
//
//
// PermanentStoragePoolList.h - owns and iterates the node's Permanent Storage Pools.
//
// Constructs one instance of every known concrete pool (currently "local" and
// "mctrivia"), holds them in an ordered, index-addressable list, and drives
// the metadata-pinning flow: when the Chain Analyzer finds new asset metadata,
// processNewMetaData() asks each pool whether the transaction belongs to it and
// schedules an IPFS download whose completion callback (_callbackNewMetadata)
// pins the appropriate files.  The list is indexed so pool 0 is "local"; other
// pools follow.  Exposes STL-style begin()/end() so callers can range-for over
// the pools.
//
// PSP_CALLBACK_NEWMETADATA_ID  - id under which _callbackNewMetadata is registered with the IPFS controller.
// PSP_PIN_METADATA_LIMIT       - max metadata size (bytes) that will be pinned locally.
//

#ifndef DIGIASSET_CORE_PERMANENTSTORAGEPOOLLIST_H
#define DIGIASSET_CORE_PERMANENTSTORAGEPOOLLIST_H

#define PSP_CALLBACK_NEWMETADATA_ID "PermanentStoragePoolList::_callbackNewMetadata"
#define PSP_PIN_METADATA_LIMIT 2000000

#include "PermanentStoragePool.h"
/**
 * Container that owns every Permanent Storage Pool the node knows about and
 * coordinates asset-metadata processing across them.  Pools are stored by
 * value-owning unique_ptr in registration order and addressed by index.
 */
class PermanentStoragePoolList {
private:
    std::vector<std::unique_ptr<PermanentStoragePool>> _pools;
    void addPool(std::unique_ptr<PermanentStoragePool> pool, const Config& config);

public:
    PermanentStoragePoolList(const std::string& configFile);
    ~PermanentStoragePoolList() = default;


    PermanentStoragePool* getRandomPool();                                                                        // returns a random non-local pool
    PermanentStoragePool* getPool(unsigned int poolIndex);                                                        // returns the pool at the given index (throws if out of range)
    PermanentStoragePool* getActiveNetworkedPool();                                                               // first SUBSCRIBED non-local pool (the one the node actually joined), or nullptr
    unsigned int getPoolCount();                                                                                  // number of pools in the list
    void processNewMetaData(const DigiByteTransaction& tx, unsigned int assetIndex, const std::string& cid);      // entry from Chain Analyzer: detect pool membership and schedule metadata download/pin
    ///public because needs to be but should only be used by PermanentStoragePoolList.cpp
    static void
    _callbackNewMetadata(const std::string& cid, const std::string& extra, const std::string& content, bool failed); // IPFS download-complete callback: parse metadata and pin the files each pool wants


    /**
     * Implement Iterator to allow list to be in foreach loop
     */
    using iterator = std::vector<std::unique_ptr<PermanentStoragePool>>::iterator;
    using const_iterator = std::vector<std::unique_ptr<PermanentStoragePool>>::const_iterator;

    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;
    /**
     * End of Iterator
     */
};




#endif //DIGIASSET_CORE_PERMANENTSTORAGEPOOLLIST_H
