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

class PoolDatabase {
public:
    explicit PoolDatabase(const std::string& dbPath);
    ~PoolDatabase();

    PoolDatabase(const PoolDatabase&) = delete;
    PoolDatabase& operator=(const PoolDatabase&) = delete;

    // Node registration. Called on keepalive and /list requests.
    // First call inserts; subsequent calls update lastSeen and payoutAddress.
    void upsertNode(const std::string& peerId,
                    const std::string& payoutAddress);

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
    unsigned int countPermanentAssets();
    unsigned int countPermanentPages();

    // Phase 3: payout support.
    // Returns (peerId, payoutAddress) pairs for nodes eligible for payout:
    // lastVerifyOk within last 24h AND verifyFails < 3 AND lastSeen within 7 days.
    struct PayoutTarget {
        std::string peerId;
        std::string payoutAddress;
    };
    std::vector<PayoutTarget> getVerifiedPayoutTargets();

    // Record a completed payout in the ledger.
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

    // peerId (multiaddr, contains the node's IP) for every node seen since the
    // given unix time. Used to geolocate nodes for the world map on the pool
    // web page.
    std::vector<std::string> getActiveNodePeerIds(int64_t sinceUnix);

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
