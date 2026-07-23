//
// Created by mctrivia on 04/11/23.
//
//
// mctrivia.h - the networked "MCTrivia's PSP" Permanent Storage Pool.
//
// Talks to a pool server over HTTP (base URL from config key `psp1server`,
// defaulting to the DigiStamp pool). Two background threads do the real work:
//   - keepAliveTask: periodically pings the server so this node stays visible
//     on the public node map.
//   - permanentFetcherTask: walks /permanent/<page>.json, pins every listed CID
//     to the local IPFS node, and opportunistically probes /list/<floor>.json
//     to report pool-registration/payout health to the dashboard.
//
// Design note: mctrivia's original protocol included an on-chain fee-matching
// payment path; in practice payouts are handled by the pool server instead, so
// the on-chain serializer path is retired (serializeMetaProcessor returns "")
// and the legacy /list endpoint is probed only for a dashboard health indicator.
// The permanent-list pinning is the core, ongoing useful work. Used by the node
// (DigiAssetWindows.exe); the pool side is served by DigiAssetPoolServer.exe.
//

#ifndef DIGIASSET_CORE_MCTRIVIA_H
#define DIGIASSET_CORE_MCTRIVIA_H



#include "PermanentStoragePool/PermanentStoragePool.h"
#include "utils.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>


/**
 * Networked PSP implementation. Overrides the PSP interface to recognise/enable
 * pool membership and forward bad-content reports over HTTP, while two owned
 * threads keep the node registered and pin the pool's permanent CID list.
 * Health/payout state observed from server probes is guarded by _healthMutex
 * and exposed through the get*Health()/getPayouts* accessors for the dashboard.
 */
class mctrivia : public PermanentStoragePool {
public:
    // Reflects what we actually know about mctrivia's server today. The
    // historical C++ code assumed an on-chain fee-matching pool protocol that
    // was never deployed server-side — see memory/project_psp_payment_diagnosis.md
    // for the full story. These values are what the dashboard and logs report.
    enum class Health {
        Unknown = 0, // not probed yet this session
        Ok      = 1, // last probe returned 2xx
        Broken  = 2  // last probe returned 5xx or threw
    };

private:
    enum ServerCalls {
        KEEP_ALIVE,
        UNSUBSCRIBE,
        REPORT
    };

    // Keepalive thread (existing).
    std::thread _keepAliveThread;
    std::atomic<bool> _keepRunning;

    // Permanent-list fetcher thread. Walks /permanent/<page>.json, pins every
    // CID via IPFS, and opportunistically probes /list/<floor>.json to report
    // pool registration health in the dashboard.
    std::thread _permanentFetcherThread;
    std::atomic<bool> _fetcherRunning;

    // Persistent identity. Read from config key `psp1secret`; generated and
    // written back to config.cfg on first run. Previously regenerated every
    // startup, which made the node look like a new identity on every restart.
    std::string _secretCode;

    // Base URL for the pool server. Read from config key `psp1server`,
    // defaults to mctrivia's original server so upstream behavior is
    // preserved. Users who want to use a different pool (e.g. a local
    // DigiAssetPoolServer.exe running on the same machine) just set
    //   psp1server=http://127.0.0.1:14028
    // in their config.cfg.
    std::string _baseUrl;

    // Permanent-list walker state.
    unsigned int _permanentPage = 23; // default to current active page on fresh install
    std::mutex _healthMutex;
    Health _registrationHealth = Health::Unknown;   // /list/<floor>.json POST
    Health _permanentFetchHealth = Health::Unknown; // /permanent/<page>.json GET
    std::string _daily;                             // "daily" field from /permanent response
    std::chrono::steady_clock::time_point _lastRegistrationProbe{};
    // Whether the pool server's last /list response said it is actively
    // distributing payouts. A pool can be reachable and accept our
    // registration (_registrationHealth == Ok) but still have payouts
    // disabled — this is the Phase 1 state of a freshly-started
    // DigiAssetPoolServer.exe. The dashboard differentiates these so
    // "Payment: active" only shows up when real DGB is flowing.
    bool _payoutsEnabled = false;
    bool _payoutsEnabledKnown = false;

    void keepAliveTask();
    void permanentFetcherTask();
    void probeListEndpoint(); // POST /list/<floor>.json once, update _registrationHealth
    bool fetchAndPinPermanentPage(unsigned int page); // returns true if page was `done`
    // updateBadList() is declared as `virtual void updateBadList()` in the
    // protected section below (upstream refactor); no private redeclaration here.
    void _callServer(ServerCalls command, const std::string& extra = "");

    std::mutex _badListMutex;
    bool _visible = true;

protected:
    std::vector<std::string> _badAssets;
    std::vector<std::string> _badFiles;
    unsigned int _badTime = 0;

    void _setConfig(const Config& config) override;
    void _reportAssetBad(const std::string& assetId) override;
    void _reportFileBad(const std::string& cid) override;
    virtual void updateBadList();

    // --- Subclass hooks. A pool at psp<index> is just mctrivia with a different
    // identity/server/price, so the new pools (digistamp, custompool) subclass
    // this and override only what differs. ---
    // "psp<index>" derived from the base _poolIndex, so each pool reads its own
    // config keys (server, secret, visible, permanentpage, pricing) without a
    // hardcoded prefix.
    std::string configPrefix() const;
    // Base URL used when psp<index>server is unset. Defaults to the DigiStamp
    // pool; custompool overrides to "" so it stays inert until configured.
    virtual std::string defaultServer() const;
    // Scale the raw size-based cost by _costPercent/100 and floor it at
    // _minCostUsdCents (converted to sats via exchangeRate). Defaults preserve
    // the historical cheap pricing (100% / no floor); tune via config, no rebuild.
    uint64_t applyPricing(uint64_t baseCost, double exchangeRate) const;
    int _costPercent = 100;            // psp<index>costpercent (100 = 1x)
    unsigned int _minCostUsdCents = 0; // psp<index>mincostcents (0 = no minimum)

public:
    mctrivia();
    ~mctrivia();

    size_t getBadAssetCount() const { return _badAssets.size(); }
    size_t getBadFileCount() const { return _badFiles.size(); }

    //called by Node Operators that subscribe to PSP
    std::string serializeMetaProcessor(const DigiByteTransaction& tx) override;                                              //always returns "" — see .cpp for why
    std::unique_ptr<PermanentStoragePoolMetaProcessor> deserializeMetaProcessor(const std::string& serializedData) override; //stub processor; never actually invoked
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

    //called by dashboard / anyone curious about pool state
    Health getRegistrationHealth();
    Health getPermanentFetchHealth();
    std::string getDailyPayoutStr();
    // True if the pool server's /list response said payoutsEnabled=true
    // AND we've seen at least one /list probe complete. When false, either
    // the probe hasn't happened yet or the pool is in Phase 1 (registration
    // accepted, no DGB flowing).
    bool getPayoutsEnabled();
    // No getPermanentPage() — only the fetcher thread touches _permanentPage,
    // and exposing it to other threads would require synchronization for no
    // user-visible benefit today.
};

// Stub metadata processor. The original mctriviaMetaProcessor used serialized-byte
// budgets from the on-chain serializer path; that path is retired (payouts run
// through the pool server), so serializeMetaProcessor returns "" and this is a
// no-op kept for interface compatibility.
class mctriviaMetaProcessor : public PermanentStoragePoolMetaProcessor {
public:
    mctriviaMetaProcessor(const std::string& serializedData, unsigned int poolIndex);
    bool _shouldPinFile(const std::string& name, const std::string& mimeType, const std::string& cid) override;
};


#endif //DIGIASSET_CORE_MCTRIVIA_H
