//
// Created by mctrivia on 04/11/23.
//
//
// PermanentStoragePoolList.cpp - constructs the pool list and runs metadata pinning.
//
// Registers the IPFS download-complete callback at load time, builds the fixed
// set of pools in the constructor (local first, then mctrivia), and provides
// index/iterator access plus pool-count and random-pool selection.  The heart
// of the file is the metadata flow: processNewMetaData() (called by the Chain
// Analyzer for each new asset metadata CID) asks every pool to serialize how
// the transaction maps into that pool, encodes the results into an "extra"
// instruction string, and schedules an IPFS download of the metadata; when the
// download finishes, _callbackNewMetadata() parses the metadata's file list and
// pins the files each matched pool wants (marking the asset as part of the pool
// only when all its files were pinnable).
//

#include "PermanentStoragePoolList.h"
#include "AppMain.h"
#include "Log.h"
#include "PermanentStoragePool/pools/local.h"
#include "PermanentStoragePool/pools/mctrivia.h"
#include "PermanentStoragePool/pools/digistamp.h"
#include "PermanentStoragePool/pools/custompool.h"
#include "static_block.hpp"
#include "utils.h"
#include <vector>

using namespace std;

// Static block to register our callback function with IPFS Controller
// Runs once at program load so the IPFS controller can invoke
// _callbackNewMetadata by name (PSP_CALLBACK_NEWMETADATA_ID) when a scheduled
// metadata download completes.
static_block {
    IPFS::registerCallback(PSP_CALLBACK_NEWMETADATA_ID, PermanentStoragePoolList::_callbackNewMetadata);
}


/*
██████╗ ██████╗ ███╗   ██╗███████╗████████╗ █████╗ ███╗   ██╗████████╗███████╗
██╔════╝██╔═══██╗████╗  ██║██╔════╝╚══██╔══╝██╔══██╗████╗  ██║╚══██╔══╝██╔════╝
██║     ██║   ██║██╔██╗ ██║███████╗   ██║   ███████║██╔██╗ ██║   ██║   ███████╗
██║     ██║   ██║██║╚██╗██║╚════██║   ██║   ██╔══██║██║╚██╗██║   ██║   ╚════██║
╚██████╗╚██████╔╝██║ ╚████║███████║   ██║   ██║  ██║██║ ╚████║   ██║   ███████║
╚═════╝ ╚═════╝ ╚═╝  ╚═══╝╚══════╝   ╚═╝   ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝   ╚══════╝
 Add new PSP to end of list.
 */
/**
 * Builds the fixed list of pools the node supports.
 *
 * Reads the config file, seeds the RNG used by getRandomPool(), then adds each
 * known pool via addPool() (which assigns its index and initializes it).  The
 * "local" pool is added first so it is always index 0; new pools must be
 * appended after it.
 *
 * @param configFile Path to the node configuration file.
 * Side effects: constructs and starts every pool (see setPoolIndexAndInitialize).
 */
PermanentStoragePoolList::PermanentStoragePoolList(const string& configFile) {
    //read the config file
    Config config(configFile);

    //make sure random number generator is seeded
    srand(static_cast<unsigned int>(time(nullptr)));

    //add known pools to list
    addPool(std::unique_ptr<PermanentStoragePool>(new local()), config);      // index 0 - local
    addPool(std::unique_ptr<PermanentStoragePool>(new mctrivia()), config);   // index 1 - legacy "MCTrivia's PSP" (deprecated; kept so existing psp1 configs keep working)
    addPool(std::unique_ptr<PermanentStoragePool>(new digistamp()), config);  // index 2 - DigiStamp pool (replacement; psp2*, opt-in)
    addPool(std::unique_ptr<PermanentStoragePool>(new custompool()), config); // index 3 - generic user-configurable slot (psp3*, opt-in, inert until psp3server set)
}


/*
██╗████████╗███████╗██████╗  █████╗ ████████╗ ██████╗ ██████╗
██║╚══██╔══╝██╔════╝██╔══██╗██╔══██╗╚══██╔══╝██╔═══██╗██╔══██╗
██║   ██║   █████╗  ██████╔╝███████║   ██║   ██║   ██║██████╔╝
██║   ██║   ██╔══╝  ██╔══██╗██╔══██║   ██║   ██║   ██║██╔══██╗
██║   ██║   ███████╗██║  ██║██║  ██║   ██║   ╚██████╔╝██║  ██║
╚═╝   ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝   ╚═╝    ╚═════╝ ╚═╝  ╚═╝
 */

PermanentStoragePoolList::iterator PermanentStoragePoolList::begin() {
    return _pools.begin();
}

PermanentStoragePoolList::iterator PermanentStoragePoolList::end() {
    return _pools.end();
}

PermanentStoragePoolList::const_iterator PermanentStoragePoolList::begin() const {
    return _pools.begin();
}

PermanentStoragePoolList::const_iterator PermanentStoragePoolList::end() const {
    return _pools.end();
}



/*
██████╗ ███████╗████████╗████████╗███████╗██████╗ ███████╗
██╔════╝ ██╔════╝╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗██╔════╝
██║  ███╗█████╗     ██║      ██║   █████╗  ██████╔╝███████╗
██║   ██║██╔══╝     ██║      ██║   ██╔══╝  ██╔══██╗╚════██║
╚██████╔╝███████╗   ██║      ██║   ███████╗██║  ██║███████║
╚═════╝ ╚══════╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝╚══════╝
 */
/**
 * Returns a PSP at random(does not return local ever)
 * @return
 */
PermanentStoragePool* PermanentStoragePoolList::getRandomPool() {
    // Generate a random index
    unsigned int index = (std::rand() % (_pools.size() - 1)) + 1;

    // Return the pool at the random index
    return _pools[index].get();
}

/**
 * Returns a specific PSP based on its index
 * @param poolIndex
 * @return
 */
PermanentStoragePool* PermanentStoragePoolList::getPool(unsigned int poolIndex) {
    // Check if the index is within the bounds of the vector
    if (poolIndex >= _pools.size()) {
        throw std::out_of_range("Pool index out of range");
    }

    // Return the pool at the specified index
    return _pools[poolIndex].get();
}

/**
 * Returns the first SUBSCRIBED non-local pool - the networked pool this node
 * actually joined (psp1/mctrivia on legacy nodes, psp2/digistamp on new ones).
 * Lets the dashboard follow whichever pool the operator is on instead of a
 * hardcoded index. Returns nullptr if the node isn't subscribed to any pool.
 */
PermanentStoragePool* PermanentStoragePoolList::getActiveNetworkedPool() {
    for (unsigned int i = 1; i < _pools.size(); i++) {
        if (_pools[i]->subscribed()) return _pools[i].get();
    }
    return nullptr;
}

/**
 * Returns how many pools there are
 * @return
 */
unsigned int PermanentStoragePoolList::getPoolCount() {
    return _pools.size();
}

/**
 * Adds a pool to the list(called by constructor only)
 * @param pool
 */
void PermanentStoragePoolList::addPool(std::unique_ptr<PermanentStoragePool> pool, const Config& config) {
    unsigned int index = static_cast<unsigned int>(_pools.size());
    pool->setPoolIndexAndInitialize(index, config);
    _pools.push_back(std::move(pool));
}

/*
██╗██████╗ ███████╗███████╗     ██████╗ █████╗ ██╗     ██╗         ██████╗  █████╗  ██████╗██╗  ██╗
██║██╔══██╗██╔════╝██╔════╝    ██╔════╝██╔══██╗██║     ██║         ██╔══██╗██╔══██╗██╔════╝██║ ██╔╝
██║██████╔╝█████╗  ███████╗    ██║     ███████║██║     ██║         ██████╔╝███████║██║     █████╔╝
██║██╔═══╝ ██╔══╝  ╚════██║    ██║     ██╔══██║██║     ██║         ██╔══██╗██╔══██║██║     ██╔═██╗
██║██║     ██║     ███████║    ╚██████╗██║  ██║███████╗███████╗    ██████╔╝██║  ██║╚██████╗██║  ██╗
╚═╝╚═╝     ╚═╝     ╚══════╝     ╚═════╝╚═╝  ╚═╝╚══════╝╚══════╝    ╚═════╝ ╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝
 */

/**
 * IPFS download-complete callback: pins the files a new asset's metadata
 * references for every pool that transaction belongs to.
 *
 * Registered under PSP_CALLBACK_NEWMETADATA_ID and invoked by the IPFS
 * controller once the metadata document downloaded by processNewMetaData() is
 * available.  It parses the downloaded JSON, requires a "data.urls" array, and
 * (if under PSP_PIN_METADATA_LIMIT bytes) pins the metadata document itself.
 * The @p extra string carries the decoding instructions built by
 * processNewMetaData in the form "a:<assetIndex>,p<poolNum>:<serialized>,...";
 * for each "p" entry it deserializes that pool's metadata processor and walks
 * the url list, pinning each IPFS file the processor says to pin (only when the
 * node is subscribed to that pool).  If every referenced file was pinnable the
 * asset is marked as part of the pool and, when subscribed, the metadata is
 * pinned too.  All exceptions are caught and logged as warnings so a bad
 * metadata document cannot abort processing.
 *
 * @param cid     CID of the downloaded metadata document.
 * @param extra   Encoded asset-index / pool-processor instructions (see above).
 * @param content The downloaded metadata document body (JSON).
 * @param failed  Download-failure flag (always false here; no timeout is set).
 * Side effects: pins IPFS content and updates pool membership in the Database.
 */
void PermanentStoragePoolList::_callbackNewMetadata(const string& cid, const string& extra, const string& content, bool failed) {
  try {
    AppMain* main = AppMain::GetInstance();
    //failed will always be false since no maxSleep ever set

    //convert content string in to processed json
    Json::Value metadata;
    Json::CharReaderBuilder rbuilder;
    istringstream s(content);
    string errs;
    if (!Json::parseFromStream(rbuilder, s, &metadata, &errs)) return; //invalid json data so don't process

    //Check if there is a data.urls section
    if (!metadata.isMember("data") || !metadata["data"].isObject()) return; //Improperly formatted
    Json::Value data = metadata["data"];
    if (!data.isMember("urls") || !data["urls"].isArray()) return; //Improperly formatted
    Json::Value urls = data["urls"];

    //pin the main metadata if less than limit
    IPFS* ipfs = main->getIPFS();
    if (content.length() < PSP_PIN_METADATA_LIMIT) {
        try { ipfs->pin(cid); } catch (...) {}
    }

    //get the list of known PSPs
    PermanentStoragePoolList* pools = main->getPermanentStoragePoolList();

    //decode what PSP this file is part of and call its processor
    //extra is in the form "a:assetIndex,p#:?,p#:?..."   where # is the pool number, ? is the encoded processor instructions.
    //assetIndex will always be first
    std::vector<std::string> pairs = utils::split(extra, ',');
    unsigned int assetIndex;
    for (const auto& pair: pairs) {
        std::vector<std::string> parts = utils::split(pair, ':');
        switch (parts[0][0]) {
            case 'a': //asset index.  Must always be first part of string
                assetIndex = stoul(parts[1]);
                break;

            case 'p': //pool.  Can be multiple pools an asset is associated with but likely only 1
                //get the processor
                unsigned int poolNumber = std::stoul(parts[0].substr(1));
                PermanentStoragePool* pool = pools->getPool(poolNumber);
                std::unique_ptr<PermanentStoragePoolMetaProcessor> processor = pool->deserializeMetaProcessor(parts[1]);
                bool pinnedAll = true;

                //Go through URLs and pin those we care about
                for (const auto& obj: urls) {
                    //get name, url, and if present mime type
                    if (!obj.isMember("name") || !obj["name"].isString()) continue;
                    if (!obj.isMember("url") || !obj["url"].isString()) continue;
                    string name = obj["name"].asString();
                    string url = obj["url"].asString();
                    if (!IPFS::isIPFSurl(url)) {
                        pinnedAll = false;
                        continue; //we can't pin urls that are not for ipfs
                    }
                    string subCID = IPFS::getCID(url);
                    string mimeType =
                            (obj.isMember("mimeType") && obj["mimeType"].isString()) ? obj["mimeType"].asString() : "";

                    //check if we should pin for this PPS
                    bool shouldPin = processor->shouldPinFile(name, mimeType, subCID);

                    //keep track of if we pinned all files
                    if (!shouldPin) {
                        pinnedAll = false;
                        continue;
                    }

                    //pin the file if subscribed to that psp
                    if (pool->subscribed()) ipfs->pin(subCID);
                }

                //if all files where pinned then mark this as part of the PSP
                if (!pinnedAll) break;
                pool->markAssetAsPartOfPool(assetIndex);

                //pin the metadata if subscribed
                if (pool->subscribed()) {
                    try { ipfs->pin(cid); } catch (...) {}
                }
                break;
        }
    }
  } catch (const std::exception& e) {
    Log::GetInstance()->addMessage(std::string("PSP metadata callback exception: ") + e.what(), Log::WARNING);
  } catch (...) {
    Log::GetInstance()->addMessage("PSP metadata callback unknown exception", Log::WARNING);
  }
}
/**
 * Chain Analyzer entry point: detect which pools an asset belongs to and
 * schedule download of its metadata for pinning.
 *
 * Asks every pool to serializeMetaProcessor(tx); each non-empty result means
 * the transaction is part of that pool and is appended to an "extra"
 * instruction string of the form "a:<assetIndex>,p<poolIndex>:<serialized>...".
 * (Some DEBUG logging dumps the detection result and the transaction outputs.)
 * It then requests an IPFS download of the metadata @p cid, passing @p extra so
 * that _callbackNewMetadata() can pin the right files once the download
 * completes.
 *
 * @param tx         The transaction that produced this asset metadata.
 * @param assetIndex Index of the asset the metadata belongs to.
 * @param cid        CID of the metadata document to download.
 * Side effects: schedules an asynchronous IPFS download; writes DEBUG log lines.
 */
void PermanentStoragePoolList::processNewMetaData(const DigiByteTransaction& tx, unsigned int assetIndex, const string& cid) {
    //compute the decoding instructions
    string extra = "a:" + to_string(assetIndex);
    for (auto& pool: *this) {
        string serialized = pool->serializeMetaProcessor(tx);               //see if part of pool
        if (serialized.empty()) continue;                                   //skip if not part of pool
        extra += ",p" + to_string(pool->getPoolIndex()) + ":" + serialized; //add to instructions
    }

    // DEBUG: surface the detection result so we can tell if serializer found
    // a matching output. "a:N" alone = no pool matched; "a:N,p1:S_..." = matched.
    // Also dump the raw outputs at DEBUG so we can see addresses/digibyte
    // values the serializer saw.
    {
        Log* log = Log::GetInstance();
        log->addMessage("PSP processNewMetaData: assetIdx=" + to_string(assetIndex) +
                        " extra=" + extra, Log::DEBUG);
        size_t outCount = tx.getOutputCount();
        for (size_t i = 0; i < outCount; i++) {
            AssetUTXO out = tx.getOutput(i);
            log->addMessage("  out[" + to_string(i) + "] addr='" + out.address +
                            "' dgb=" + to_string(out.digibyte), Log::DEBUG);
        }
    }

    //download the metadata
    IPFS* ipfs = AppMain::GetInstance()->getIPFS();
    ipfs->callOnDownload(cid, "", extra, PSP_CALLBACK_NEWMETADATA_ID);
}
