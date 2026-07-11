//
// PoolDatabase - sqlite wrapper for the pool server's persistent state.
//
// Stores the set of registered nodes (peerId -> payout address), the
// canonical permanent-assets list (one row per assetId/txhash/cid with a
// page number), the payouts ledger, and pool config. Uses the sqlite3
// amalgamation already in src/sqlite3.c.
//

#ifndef DIGIASSET_POOL_DATABASE_H
#define DIGIASSET_POOL_DATABASE_H

#include "sqlite3.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Thread-safe sqlite wrapper holding all durable pool-server state: the
// registered-node table (payout address, first/last seen, dial-back
// verification counters, last observed IP), the permanent-assets list and its
// per-page done/daily metadata, the payouts ledger, and a key/value runtime
// config store. Every public method takes an internal mutex, so it is safe to
// call from the HTTP worker threads, the verifier thread, and the dashboard
// thread concurrently. Builds/migrates its schema on construction.
class PoolDatabase {
public:
    explicit PoolDatabase(const std::string& dbPath);
    ~PoolDatabase();

    PoolDatabase(const PoolDatabase&) = delete;
    PoolDatabase& operator=(const PoolDatabase&) = delete;

    // Node registration. Called on keepalive and /list requests.
    // First call inserts; subsequent calls update lastSeen and payoutAddress.
    // observedIp is the node's public IP as seen at registration (used to
    // geolocate it for the world map); a blank observedIp leaves any
    // previously stored address untouched.
    // A node's `secret` is bound on first registration. A later registration for
    // the same peerId may only change payoutAddress if it presents the matching
    // secret - this stops a payout-address takeover by re-POSTing a victim's
    // peerId (audit M1). A mismatched secret is rejected entirely.
    void upsertNode(const std::string& peerId,
                    const std::string& payoutAddress,
                    const std::string& secret = "",
                    const std::string& observedIp = "");

    // Dial-back verification state. The PoolVerifier thread calls these
    // after each probe attempt.
    //   recordVerifySuccess: bumps lastVerifyOk, resets verifyFails to 0
    //   recordVerifyFailure: increments verifyFails (lastVerifyOk unchanged)
    void recordVerifySuccess(const std::string& peerId);
    void recordVerifyFailure(const std::string& peerId);

    // Returns up to `limit` peerIds the verifier should probe next,
    // ordered by lastVerifyOk ascending (least-recently-verified first).
    // Used by PoolVerifier to pick work each iteration.
    std::vector<std::string> getPeerIdsForVerification(unsigned int limit);

    // Counts for the dashboard Verified: row.
    unsigned int countVerifiedSince(int64_t unixSeconds);
    unsigned int countFailedOut(); // nodes with verifyFails >= 3

    // Permanent assets (one row per (assetId, txHash, cid) tuple).
    // Used by the first-run snapshot and, later, operator-added entries.
    void insertPermanentAsset(const std::string& assetId,
                              const std::string& txHash,
                              const std::string& cid,
                              unsigned int page);

    // Mark a permanent page as done=true. The snapshot sets this based on
    // whether mctrivia's source page had done=true at fetch time.
    void setPermanentPageDone(unsigned int page, bool done, const std::string& daily);

    // Pick the page an operator/marketplace-added asset should be written to.
    // Returns the highest page that is NOT yet marked done, so clients (which
    // walk pages upward and park on the first non-done page, re-fetching it
    // each cycle) pick up new additions without a restart. Rolls to a fresh
    // page — marking the current one done — once it fills, to keep page bodies
    // a sane size. On an empty pool it returns the stock client start page (23)
    // so a default client's walk actually reaches these entries.
    unsigned int getWritablePage();

    // Has the first-run snapshot already populated the db?
    bool hasPermanentData();

    // Return up to `limit` random CIDs from the permanent list. Used by the
    // verifier's NAT-tolerant fallback: it runs findprovs on these to see
    // whether a NAT'd node has announced itself as a provider of content it
    // is supposed to be pinning.
    std::vector<std::string> getSampleCids(unsigned int limit);

    // Build the JSON body for /permanent/<page>.json: the {changes, daily, done}
    // shape mctrivia's server uses, serialized as a string the HTTP handler
    // can send straight to the client.
    std::string buildPermanentPageJson(unsigned int page);

    // Build the JSON body for /nodes.json - array of {id: peerId} objects
    // for every node seen within the last 7 days.
    std::string buildNodesJson();

    // Build the JSON body for /map.json - for now, one empty-geo entry per
    // known node so the existing dashboard's node count isn't wrong.
    std::string buildMapJson();

    // Dashboard counters.
    unsigned int countNodesSeenSince(int64_t unixSeconds);
    unsigned int countTotalNodes();
    unsigned int countActiveNodes();   // seen in last 7d AND verifyFails < 3 (matches /nodes.json + map)
    unsigned int countPermanentAssets();
    unsigned int countPermanentPages();

    // Phase 3: payout support.
    // Returns (peerId, payoutAddress) pairs for nodes eligible for payout:
    // lastVerifyOk within last 24h AND verifyFails < 3 AND lastSeen within 7 days.
    struct PayoutTarget {
        std::string peerId;
        std::string payoutAddress;
        double coverage    = 0.0;   // 0..1 EMA: share of sampled CIDs this node provides
        double reliability = 0.0;   // 0..1 EMA: share of verify rounds it passed
        double weight      = 0.0;   // coverage * reliability - the payout weight
    };
    std::vector<PayoutTarget> getVerifiedPayoutTargets();

    // Fairness scoring: fold one verify round's observation into a node's
    // coverage + reliability EMAs. coverageObs is that round's hit-rate (0..1),
    // applied only if hasCoverageSample (skip on a measurement gap so it doesn't
    // penalize the node); verifyOk is whether it passed reachability this round.
    void updateNodeScores(const std::string& peerId, double coverageObs, bool hasCoverageSample, bool verifyOk);

    // Record a completed payout in the ledger.
    // Record a payout whose send RESULT was ambiguous (RPC timeout after the tx
    // may have broadcast): a pending ledger row (paidTxid NULL) that advances the
    // once-per-period guard so a re-run can't blindly double-pay. (audit M7)
    void recordPendingPayout(const std::string& payoutAddress, int64_t amountDgbSat);
    void recordPayout(const std::string& payoutAddress, int64_t amountDgbSat,
                      const std::string& txid);

    // Ledger totals for dashboard display.
    double getPaidTotalDgb();        // sum of all paid-out DGB
    unsigned int getPaidCount();     // number of payout transactions

    // Unix time of the most recent successful payout (0 if none). Used to
    // enforce the once-per-period spend guard so pressing [E] twice in quick
    // succession can't double-pay.
    int64_t getLastPayoutAt();

    // Recent payouts for the public ledger on the pool web page. Newest first.
    struct PayoutRow {
        std::string payoutAddress;
        int64_t amountDgbSat = 0;
        int64_t paidAt = 0;
        std::string txid;
    };
    std::vector<PayoutRow> getRecentPayouts(unsigned int limit);

    // Observed public IP for every node seen since the given unix time (skips
    // rows with no recorded address). Used to geolocate nodes for the world map
    // on the pool web page. The IP is captured from the node's registration
    // connection, not derived from its peerId.
    std::vector<std::string> getActiveNodeIps(int64_t sinceUnix);

    // Pool-local config key/value store (separate from the operator's
    // editable pool.cfg; this is runtime state like "last snapshot time").
    void setConfig(const std::string& key, const std::string& value);
    std::string getConfig(const std::string& key, const std::string& defaultValue = "");

private:
    sqlite3* _db = nullptr;
    std::mutex _mutex;
    void exec(const char* sql);
    void buildSchema();
};

#endif // DIGIASSET_POOL_DATABASE_H
