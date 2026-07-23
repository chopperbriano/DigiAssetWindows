//
// Created by mctrivia on 04/11/23.
//
//
// mctrivia.cpp - implementation of the networked Permanent Storage Pool client
// (see mctrivia.h), named for mctrivia who designed this elegant pool protocol.
// Contains the two background thread bodies (keepalive + permanent fetcher), the
// HTTP glue to the pool server, cost/enable logic for asset creators, and the
// bad-list maintenance. Registration + payouts are handled by whichever pool you
// point psp1server at (the community DigiStamp pool by default, or your own); the
// legacy /list endpoint is probed only for diagnostics.
//

#include "mctrivia.h"
#include "AppMain.h"
#include "Config.h"
#include "CurlHandler.h"
#include "IPFS.h"
#include "Log.h"
#include "DigiAssetConstants.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <set>
#include <sstream>
#include <thread>
using namespace std;

namespace {
    // Default base URL for the pool server used when `psp1server` is not set in
    // config.cfg. mctrivia's DigiAssetX pool (https://ipfs.digiassetx.com)
    // pioneered permanent storage and the payout protocol this builds on - huge
    // credit to that original work. The Windows fork simply defaults to the
    // community DigiStamp pool, and you can point at ANY pool - including your
    // own - by setting `psp1server` (e.g. `http://127.0.0.1:14028` for a local
    // DigiAssetPoolServer.exe). Keep this in sync with the wizard default in
    // src/main.cpp.
    const std::string DEFAULT_POOL_BASE = "https://pool.digistamp.co";
}

mctrivia::mctrivia() : _keepRunning(false), _fetcherRunning(false) {}
mctrivia::~mctrivia() { stop(); }



string mctrivia::getName() {
    return "MCTrivia's PSP";
}
string mctrivia::getDescription() {
    return "Originally operated by digiassetX Inc and continued to run by Matthew Cornelisse.  This pool makes sure asset metadata is always available and pays others DigiAsset Core nodes to help distribute the metadata.";
}
string mctrivia::getURL() {
    return _baseUrl.empty() ? defaultServer() : _baseUrl;
}

// psp<index> config prefix, derived from the base-assigned pool index so each
// pool reads its own keys without a hardcoded prefix.
string mctrivia::configPrefix() const {
    return "psp" + std::to_string(_poolIndex);
}

// Base pool server URL when psp<index>server is unset. mctrivia + digistamp use
// the DigiStamp pool; custompool overrides to "" (inert until configured).
string mctrivia::defaultServer() const {
    return DEFAULT_POOL_BASE;
}

// Apply the configurable pricing knobs to the raw size-based cost. Defaults
// (100% / no floor) preserve the historical cheap pricing.
uint64_t mctrivia::applyPricing(uint64_t baseCost, double exchangeRate) const {
    if (baseCost == 0) return 0;
    uint64_t cost = static_cast<uint64_t>(baseCost * (_costPercent / 100.0));
    if (_minCostUsdCents > 0) {
        // exchangeRate is DGB sats per USD; convert the cents floor to sats.
        uint64_t minSat = static_cast<uint64_t>(ceil(_minCostUsdCents / 100.0 * exchangeRate));
        if (cost < minSat) cost = minSat;
    }
    return cost;
}

/**
 * Returns the cost of including in psp.
 * May throw DigiAsset::exceptionInvalidMetaData, or PermanentStoragePool::exceptionCantEnablePSP()
 * @param tx
 * @return in DGB sats
 */
uint64_t mctrivia::getCost(const DigiByteTransaction& tx) {
    //check if even an issuance tx
    if (!tx.isIssuance()) return 0;

    uint64_t size = 0;

    //find the metadata and get its size
    DigiAsset asset = tx.getIssuedAsset();
    string cid = asset.getCID();
    IPFS* ipfs = AppMain::GetInstance()->getIPFS();
    try {
        size += ipfs->getSize(cid);
    } catch (...) {
        return 0; // Can't determine size — assume free or skip
    }

    //download the metadata and decode it
    string metadataStr = ipfs->callOnDownloadSync(cid);
    Json::CharReaderBuilder rbuilder;
    Json::Value metadata;
    istringstream s(metadataStr);
    string errs;
    if (!Json::parseFromStream(rbuilder, s, &metadata, &errs)) throw DigiAsset::exceptionInvalidMetaData(); //Improperly formatted

    //Check if there is a data.urls section
    if (!metadata.isMember("data") || !metadata["data"].isObject()) throw DigiAsset::exceptionInvalidMetaData(); //Improperly formatted
    Json::Value data = metadata["data"];
    if (!data.isMember("urls") || !data["urls"].isArray()) throw DigiAsset::exceptionInvalidMetaData(); //Improperly formatted
    Json::Value urls = data["urls"];

    //Go through URLs and get the size of the ones we care about
    for (const auto& obj: urls) {
        //Ignore all links that don't have name and url
        if (!obj.isMember("name") || !obj["name"].isString()) continue;
        if (!obj.isMember("url") || !obj["url"].isString()) continue;
        string name = obj["name"].asString();
        string url = obj["url"].asString();

        //Ignore links not on IPFS
        if (!ipfs->isIPFSurl(url)) throw PermanentStoragePool::exceptionCantEnablePSP(); //Not on IPFS so can't be included in the PSP

        //add to size value
        try {
            size += ipfs->getSize(url.substr(7));
        } catch (...) {
            // Can't size — skip
        }
    }

    //convert size to DGB sats: $1.20 USD per MB, exchange rate is DGB sats per US dollar.
    //Must stay the exact inverse of serializeMetaProcessor's
    //   bytes = 1000000 * dgb / (exchangeRate * 1.2)
    //or pay ins won't buy the number of bytes the pool expects
    Database* db = AppMain::GetInstance()->getDatabase();
    double exchangeRate = db->getCurrentExchangeRate(DigiAssetConstants::standardExchangeRates[1]); //USD
    uint64_t baseCost = static_cast<uint64_t>(ceil(size * 1.2 * exchangeRate / 1000000.0));
    return applyPricing(baseCost, exchangeRate);
}

/**
 * Throws
 *      DigiAsset::exceptionInvalidMetaData - Asset meta data is improperly formed
 *      PermanentStoragePool::exceptionCantEnablePSP - transaction is not an issuance
 *      DigiByteTransaction::exceptionNotEnoughFunds - not enough funds left in the inputs to pay for added output
 * @param tx
 */
void mctrivia::enable(DigiByteTransaction& tx) {
    //make sure transaction is even an issuance
    if (!tx.isIssuance()) throw exceptionCantEnablePSP();

    //get cost
    uint64_t cost = getCost(tx);

    //an output below the dust threshold would make the whole tx unrelayable.  Overpaying
    //slightly is harmless(pay in simply buys more storage than needed)
    if (cost < DigiAssetConstants::DIGIBYTE_DUST) cost = DigiAssetConstants::DIGIBYTE_DUST;

    //check if there is already an output to output address:
    const string outputAddress = "dgb1qjnzadu643tsfzjqjydnh06s9lgzp3m4sg3j68x";
    for (size_t i = 0; i < tx.getOutputCount(); i++) {
        if (tx.getOutput(i).address == outputAddress) throw exceptionCantEnablePSP(); //extremely unlikely so don't bother handling the edge case
    }

    //create an output transaction and try to add it to the tx
    tx.addDigiByteOutput(outputAddress, cost);
}
/**
 * Loads this pool's configuration from config.cfg. Reads visibility
 * (psp1visible), the pool server base URL (psp1server, trailing slashes
 * stripped), a persistent node identity (psp1secret, generated and written back
 * on first run), and the permanent-list walker page (psp1permanentpage).
 * Side effect: may write a freshly generated psp1secret back to config.cfg.
 */
void mctrivia::_setConfig(const Config& config) {
    _visible = config.getBool(configPrefix() + "visible", true);

    // Pool server base URL. Defaults to defaultServer() (the DigiStamp pool for
    // mctrivia/digistamp). Set psp<index>server=http://... in config.cfg to point
    // at a different pool - e.g. a local DigiAssetPoolServer.exe you run yourself
    // for full control over registration + payouts.
    _baseUrl = config.getString(configPrefix() + "server", defaultServer());
    // Strip any trailing slash so later url construction like `_baseUrl +
    // "/permanent/..."` produces clean paths regardless of how the user
    // wrote the config value.
    while (!_baseUrl.empty() && _baseUrl.back() == '/') _baseUrl.pop_back();

    // Persistent node identity. Historically this was a fresh random string on
    // every startup — harmless-looking but meant the server saw us as a new
    // identity each restart. Read from config; generate + persist on first run.
    _secretCode = config.getString(configPrefix() + "secret", "");
    if (_secretCode.empty()) {
        _secretCode = utils::generateRandom(8, utils::CodeType::ALPHANUMERIC);
        try {
            Config writable("config.cfg");
            writable.setString(configPrefix() + "secret", _secretCode);
            writable.write();
        } catch (...) {
            Log::GetInstance()->addMessage(
                    "Could not persist " + configPrefix() + "secret to config.cfg; will regenerate next run",
                    Log::WARNING);
        }
    }

    // Permanent-list walker page. Defaults to 23 (the currently active page as
    // of 2026-04-11). Advancing happens in permanentFetcherTask.
    _permanentPage = static_cast<unsigned int>(config.getInteger(configPrefix() + "permanentpage", 23));

    // Configurable pricing. Defaults preserve the historical cheap pricing
    // (100% of the size-based cost, no minimum). Raise psp<index>costpercent
    // (e.g. 10000 = 100x) or set psp<index>mincostcents (e.g. 100 = $1 floor).
    _costPercent = config.getInteger(configPrefix() + "costpercent", 100);
    _minCostUsdCents = static_cast<unsigned int>(config.getInteger(configPrefix() + "mincostcents", 0));
}

/**
 * Starts the background threads. One keepalive thread (pings the server every
 * 20 min), one fetcher thread (walks /permanent/<page>.json and probes /list).
 */
void mctrivia::start() {
    if (!_keepRunning.load()) {
        _keepRunning.store(true);
        _keepAliveThread = std::thread(&mctrivia::keepAliveTask, this);
    }
    if (!_fetcherRunning.load()) {
        _fetcherRunning.store(true);
        _permanentFetcherThread = std::thread(&mctrivia::permanentFetcherTask, this);
    }
}

/**
 * Stops the background threads.
 */
void mctrivia::stop() {
    _keepRunning.store(false);
    _fetcherRunning.store(false);
    if (_keepAliveThread.joinable()) {
        _keepAliveThread.join();
    }
    if (_permanentFetcherThread.joinable()) {
        _permanentFetcherThread.join();
    }
}

/**
 * Lets the server know we are sharing data. The /keepalive endpoint always
 * echoes "unsubscribe failed will time out anyways" regardless of state — it's
 * an activity ping and nothing more. We still call it to remain visible on the
 * public node map.
 */
void mctrivia::keepAliveTask() {
    Log* log = Log::GetInstance();
    log->addMessage("PSP keepalive thread started (will ping every 20 minutes)", Log::INFO);
    while (_keepRunning.load()) {
        //make keep alive request
        try {
            _callServer(KEEP_ALIVE);
        } catch (const std::exception& e) {
            log->addMessage("PSP keepalive task exception: " + std::string(e.what()), Log::WARNING);
        } catch (...) {
            log->addMessage("PSP keepalive task: unknown exception", Log::WARNING);
        }

        //sleep for 20 minutes, but wake up periodically so shutdown is responsive
        for (int i = 0; i < 120 && _keepRunning.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
}

/**
 * Walks the /permanent/<page>.json list and pins every CID to IPFS. When a
 * page reports `done: true`, advances to the next page and persists progress
 * to config.cfg so restarts don't re-walk from scratch.
 *
 * This is the core "what should this node pin" signal in mctrivia's protocol,
 * and the pinning is genuinely useful work regardless of which pool handles
 * registration + payouts (that's whatever pool you point psp1server at).
 */
void mctrivia::permanentFetcherTask() {
    Log* log = Log::GetInstance();
    log->addMessage("PSP permanent fetcher thread started (page=" +
                    std::to_string(_permanentPage) + ")",
                    Log::INFO);

    int probeCounter = 0;

    while (_fetcherRunning.load()) {
        // Fetch current page. If `done` comes back true, advance and persist.
        try {
            bool done = fetchAndPinPermanentPage(_permanentPage);
            if (done) {
                _permanentPage++;
                try {
                    Config writable("config.cfg");
                    writable.setInteger("psp1permanentpage", static_cast<int>(_permanentPage));
                    writable.write();
                } catch (...) {
                    // Non-fatal — worst case we re-walk the page next restart.
                }
                log->addMessage("PSP permanent page " +
                                std::to_string(_permanentPage - 1) +
                                " complete, advancing to " +
                                std::to_string(_permanentPage),
                                Log::INFO);
            }
        } catch (const std::exception& e) {
            log->addMessage(std::string("PSP permanent fetch exception: ") + e.what(),
                            Log::WARNING);
            std::lock_guard<std::mutex> lk(_healthMutex);
            _permanentFetchHealth = Health::Broken;
        }

        // Probe the /list endpoint once per 3 iterations (~30 min). Purely
        // diagnostic: registration + payouts are handled by whichever pool you
        // point psp1server at, so this legacy endpoint's response just drives a
        // dashboard indicator - if it responds, the dashboard reflects it with no
        // client update. Wrapped in its own try/catch so a transport error can
        // never take down the fetcher thread.
        if ((probeCounter % 3) == 0) {
            try {
                probeListEndpoint();
            } catch (const std::exception& e) {
                log->addMessage(std::string("PSP /list probe exception: ") + e.what(),
                                Log::DEBUG);
            } catch (...) {
                log->addMessage("PSP /list probe unknown exception", Log::DEBUG);
            }
        }
        probeCounter++;

        // Sleep ~10 minutes between iterations, checking stop flag every 10s.
        for (int i = 0; i < 60 && _fetcherRunning.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
}

/**
 * Returns true if the page came back with `done: true`, meaning the caller
 * should advance to the next page. False otherwise (page still growing, or
 * fetch failed — either way, don't advance).
 */
bool mctrivia::fetchAndPinPermanentPage(unsigned int page) {
    Log* log = Log::GetInstance();
    const std::string url = _baseUrl + "/permanent/" + std::to_string(page) + ".json";

    std::string body;
    try {
        body = CurlHandler::get(url, 30000);
    } catch (const std::exception& e) {
        log->addMessage("PSP permanent GET failed (" + url + "): " + e.what(),
                        Log::WARNING);
        std::lock_guard<std::mutex> lk(_healthMutex);
        _permanentFetchHealth = Health::Broken;
        return false;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(body, root)) {
        log->addMessage("PSP permanent page " + std::to_string(page) +
                        " returned non-JSON (len=" + std::to_string(body.size()) + ")",
                        Log::WARNING);
        std::lock_guard<std::mutex> lk(_healthMutex);
        _permanentFetchHealth = Health::Broken;
        return false;
    }

    // Server error envelope: {"error":"..."}. Means the page doesn't exist yet
    // on the server (walker advanced past the frontier). Do NOT roll back —
    // the previous page is already `done:true` and rolling back would make us
    // re-pin its entire CID list on every iteration in a loop. Instead, stay
    // at this page and retry next iteration; the server will eventually start
    // populating it.
    if (root.isMember("error")) {
        log->addMessage("PSP permanent page " + std::to_string(page) +
                        " not ready on server: " + root["error"].asString(),
                        Log::DEBUG);
        std::lock_guard<std::mutex> lk(_healthMutex);
        _permanentFetchHealth = Health::Broken;
        return false;
    }

    // Extract daily payout counter if present (display-only; surfaces in dashboard).
    if (root.isMember("daily") && root["daily"].isString()) {
        std::lock_guard<std::mutex> lk(_healthMutex);
        _daily = root["daily"].asString();
    }

    // Walk the changes map. Each key is "<assetId>-<txhash>", value is a list
    // of CIDs that belong to that asset. Pin every CID via the local IPFS node.
    // Use the null-safe accessor in case we raced shutdown.
    IPFS* ipfs = AppMain::GetInstance()->getIPFSIfSet();
    if (!ipfs) {
        std::lock_guard<std::mutex> lk(_healthMutex);
        _permanentFetchHealth = Health::Broken;
        return false;
    }
    unsigned int pinAttempts = 0;
    unsigned int pinFailures = 0;

    if (root.isMember("changes") && root["changes"].isObject()) {
        const Json::Value& changes = root["changes"];
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            const Json::Value& cidList = *it;
            if (!cidList.isArray()) continue;
            for (const auto& cidVal: cidList) {
                if (!cidVal.isString()) continue;
                const std::string cid = cidVal.asString();
                if (cid.empty()) continue;
                pinAttempts++;
                try {
                    ipfs->pin(cid);
                } catch (...) {
                    pinFailures++;
                    // Don't log each failure at INFO — there are 60-100 CIDs
                    // per page and IPFS can legitimately fail to find some.
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(_healthMutex);
        _permanentFetchHealth = Health::Ok;
    }

    log->addMessage("PSP permanent page " + std::to_string(page) +
                            " fetched (" + std::to_string(pinAttempts) + " CIDs, " +
                            std::to_string(pinFailures) + " pin failures)",
                    Log::DEBUG);

    bool done = root.isMember("done") && root["done"].isBool() && root["done"].asBool();
    return done;
}

/**
 * POSTs to /list/<floor>.json once. Best-effort - this legacy endpoint may or
 * may not respond depending on the pool in use; either way we record the result
 * for the dashboard indicator.
 */
void mctrivia::probeListEndpoint() {
    Log* log = Log::GetInstance();

    // Figure out a valid height floor. We use the chain analyzer's current
    // height if available, otherwise a sensible default.
    unsigned int height = 23301643;
    try {
        DigiByteCore* dgb = AppMain::GetInstance()->getDigiByteCoreIfSet();
        if (dgb) height = dgb->getBlockCount();
    } catch (...) {
        // Use the default — we just want to send a plausible value.
    }
    unsigned int floorHeight = (height / 100000) * 100000;

    IPFS* ipfs = AppMain::GetInstance()->getIPFSIfSet();
    std::string peerId;
    if (ipfs) {
        try {
            peerId = ipfs->getPeerId();
        } catch (...) {}
    }

    // Build minimal JSON body matching digiassetX/digiasset_node syncer.js shape.
    std::ostringstream body;
    body << "{\"height\":" << (height + 1)
         << ",\"version\":5"
         << ",\"show\":true";
    if (!peerId.empty()) {
        body << ",\"peerId\":\"" << peerId << "\"";
    }
    std::string payout = getPayoutAddress();
    if (!payout.empty()) {
        body << ",\"payout\":\"" << payout << "\"";
    }
    // Include our persistent identity secret so the pool can bind this peerId to
    // us on first registration and reject any later payout-address change that
    // doesn't present the matching secret (payout-address takeover guard, M1).
    if (!_secretCode.empty()) {
        body << ",\"secret\":\"" << _secretCode << "\"";
    }
    body << "}";

    const std::string url = _baseUrl + "/list/" + std::to_string(floorHeight) + ".json";
    std::string respBody;
    long status = 0;
    try {
        status = CurlHandler::postJson(url, body.str(), respBody, 15000);
    } catch (const std::exception& e) {
        log->addMessage("PSP /list probe transport error: " + std::string(e.what()),
                        Log::DEBUG);
        std::lock_guard<std::mutex> lk(_healthMutex);
        _registrationHealth = Health::Broken;
        _lastRegistrationProbe = std::chrono::steady_clock::now();
        return;
    }

    log->addMessage("PSP /list probe: HTTP " + std::to_string(status) +
                            " body=" + respBody,
                    Log::DEBUG);

    // Parse payoutsEnabled out of the response body if present. Our own
    // pool server returns `{"payoutsEnabled":false,"phase":1,...}` as a
    // Phase 1 default and `{"payoutsEnabled":true,"phase":3,...}` once the
    // operator has flipped poolpayouts=1. Mctrivia's legacy server doesn't
    // include this field at all; absence means "unknown, assume false".
    bool parsedPayoutsEnabled = false;
    if (status >= 200 && status < 300) {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(respBody, root)) {
            if (root.isMember("payoutsEnabled") && root["payoutsEnabled"].isBool()) {
                parsedPayoutsEnabled = root["payoutsEnabled"].asBool();
            }
        }
    }

    std::lock_guard<std::mutex> lk(_healthMutex);
    if (status >= 200 && status < 300) {
        _registrationHealth = Health::Ok;
        _payoutsEnabled = parsedPayoutsEnabled;
        _payoutsEnabledKnown = true;
    } else {
        _registrationHealth = Health::Broken;
        _payoutsEnabled = false;
        _payoutsEnabledKnown = false;
    }
    _lastRegistrationProbe = std::chrono::steady_clock::now();
}

/**
 * Dead code: the 14 hard-coded pay-in addresses and the bytes-paid math were
 * an attempt to implement a fee-matching PSP protocol that was never actually
 * deployed server-side. No transaction on-chain has ever paid those addresses.
 * Returning "" means this pool never claims any issuance — which is correct:
 * real asset-to-pool matching happens via the /permanent/<page>.json list
 * fetched by the background thread, not via on-chain output scanning.
 */
string mctrivia::serializeMetaProcessor(const DigiByteTransaction& /*tx*/) {
    return "";
}

/**
 * Stub. Never reached because serializeMetaProcessor always returns "".
 */
unique_ptr<PermanentStoragePoolMetaProcessor> mctrivia::deserializeMetaProcessor(const string& serializedData) {
    return unique_ptr<PermanentStoragePoolMetaProcessor>(new mctriviaMetaProcessor(serializedData, _poolIndex));
}

/**
 * Function to make calls to PSP monitoring server
 * @param command
 * @return
 */
void mctrivia::_callServer(ServerCalls command, const string& extra) {
    Log* log = Log::GetInstance();
    string commandStr;
    string address = "NA";
    switch (command) {
        case UNSUBSCRIBE:
            commandStr = "unsubscribe";
            break;
        case KEEP_ALIVE:
            commandStr = "keepalive";
            address = getPayoutAddress();
            break;
        case REPORT:
            commandStr = "report";
            break;
    }

    //get values inside loop in case they have changed
    IPFS* ipfs = AppMain::GetInstance()->getIPFS();
    string peerId = ipfs->getPeerId();
    // Note: getPeerId() returns a full multiaddress like /ip4/X/tcp/Y/p2p/12D3Koo...
    // The server expects this format (matches upstream Linux behavior).
    string url = _baseUrl + "/" + commandStr;
    if (!extra.empty()) url += "/" + extra;

    // Diagnostic detail at DEBUG level only — visible after pressing [L] in the
    // dashboard. Includes the secret, so we never want it at INFO.
    if (command == KEEP_ALIVE) {
        log->addMessage("PSP keepalive REQUEST: url=" + url, Log::DEBUG);
        log->addMessage("PSP keepalive REQUEST body: address=" + address +
                        "&peerId=" + peerId +
                        "&visible=" + (_visible ? std::string("v") : std::string("h")) +
                        "&secret=" + _secretCode, Log::DEBUG);
    }

    std::string response;
    try {
        response = CurlHandler::post(url, {{"address", address},
                                {"peerId", peerId},
                                {"visible", (_visible ? "v" : "h")},
                                {"secret", _secretCode}});
    } catch (const std::exception& e) {
        if (command == KEEP_ALIVE) {
            log->addMessage("PSP keepalive FAILED: " + std::string(e.what()), Log::WARNING);
        }
        throw;
    }

    if (command == KEEP_ALIVE) {
        // The server's `keepalive` endpoint always returns
        //   {"error":"unsubscribe failed will time out anyways"}
        // for both successful and unsuccessful keepalives — confirmed by direct
        // curl from a working Linux node that DOES receive payments. The text
        // is misleading; treat it as the expected OK response and only flag
        // anything else as an actual problem.
        const std::string expectedOk = "unsubscribe failed will time out anyways";
        bool responseOk = (response.find(expectedOk) != std::string::npos);

        log->addMessage("PSP keepalive RESPONSE: " + response, Log::DEBUG);
        if (responseOk) {
            // Use _baseUrl, not the hardcoded ipfs.digiassetx.com, so users
            // running their own DigiAssetPoolServer see it reflected here.
            log->addMessage("Reported online to " + _baseUrl + " (server id: " +
                            peerId + ")", Log::DEBUG);
        } else {
            log->addMessage("PSP keepalive returned UNEXPECTED response: " + response,
                            Log::WARNING);
        }
    }

    //update the bad list
    updateBadList();
}
/**
 * Returns true if assetId is on the pool's bad list. Refreshes the cached bad
 * list from the server (updateBadList) when it is older than 20 minutes, then
 * checks membership.
 */
bool mctrivia::isAssetBad(const std::string& assetId) {
    //make sure bad list is populated(there are known bad assets so an empty list means we have not checked yet)
    unsigned int currentTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (currentTime - _badTime > 1200) updateBadList();

    //see if assetIndex in bad list
    std::lock_guard<std::mutex> lock(_badListMutex);
    auto it = find(_badAssets.begin(), _badAssets.end(), assetId);
    return it != _badAssets.end();
}
/**
 * Forwards a bad-asset report to the pool server via the REPORT call.
 * @throws exceptionCouldntReport if the server call fails
 */
void mctrivia::_reportAssetBad(const std::string& assetId) {
    //send to server
    try {
        _callServer(REPORT, assetId);
    } catch (...) {
        throw exceptionCouldntReport();
    }
}

/**
 * Fetches /bad.json from the pool server and merges any newly listed bad assets
 * and bad files into the local caches, calling reportAssetBad/reportFileBad
 * (internalOnly) so the node unpins/flags them without re-notifying the server.
 * Only unseen ids are added, so repeated polls don't grow the caches unbounded.
 * Updates _badTime on success; failures are swallowed and logged at DEBUG.
 */
void mctrivia::updateBadList() {
    std::lock_guard<std::mutex> lock(_badListMutex);
    try {
        //make curl request
        const string url = _baseUrl + "/bad.json";
        string readBuffer = CurlHandler::get(url);

        //convert to json object
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(readBuffer, root)) return;

        //save bad assets. Upstream had an unconditional second push_back after
        //this loop body that caused _badAssets to grow unbounded on every poll.
        //Now: only push if we haven't seen it before, same as _badFiles should do.
        for (const Json::Value& value: root["assets"]) {
            string assetId = value.asString();
            if (find(_badAssets.begin(), _badAssets.end(), assetId) == _badAssets.end()) {
                reportAssetBad(assetId, true);
                _badAssets.push_back(assetId);
            }
        }

        //save bad files (same fix as above)
        for (const Json::Value& value: root["cids"]) {
            string cid = value.asString();
            if (find(_badFiles.begin(), _badFiles.end(), cid) == _badFiles.end()) {
                reportFileBad(cid, true);
                _badFiles.push_back(cid);
            }
        }

        //update bad list time
        _badTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    } catch (const exception& e) {
        Log* log = Log::GetInstance();
        log->addMessage("Failed to load bad list for mctrivia bad list", Log::DEBUG);
    }
}
/**
 * Forwards a bad-file report to the pool server via the REPORT call.
 * @throws exceptionCouldntReport if the server call fails
 */
void mctrivia::_reportFileBad(const string& cid) {
    //send to server
    try {
        _callServer(REPORT, cid);
    } catch (...) {
        throw exceptionCouldntReport();
    }
}

mctrivia::Health mctrivia::getRegistrationHealth() {
    std::lock_guard<std::mutex> lk(_healthMutex);
    return _registrationHealth;
}
mctrivia::Health mctrivia::getPermanentFetchHealth() {
    std::lock_guard<std::mutex> lk(_healthMutex);
    return _permanentFetchHealth;
}
bool mctrivia::getPayoutsEnabled() {
    std::lock_guard<std::mutex> lk(_healthMutex);
    return _payoutsEnabledKnown && _payoutsEnabled;
}
std::string mctrivia::getDailyPayoutStr() {
    std::lock_guard<std::mutex> lk(_healthMutex);
    return _daily;
}

// Stub metadata processor — never actually reached because the concrete
// mctrivia::serializeMetaProcessor returns "" unconditionally. Kept so the
// abstract base class can still be concretely satisfied at link time.
mctriviaMetaProcessor::mctriviaMetaProcessor(const string& /*serializedData*/, unsigned int poolIndex) : PermanentStoragePoolMetaProcessor(poolIndex) {
    _poolIndex = poolIndex;
}

bool mctriviaMetaProcessor::_shouldPinFile(const std::string& /*name*/, const std::string& /*mimeType*/, const std::string& /*cid*/) {
    return false;
}
