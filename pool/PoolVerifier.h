//
// PoolVerifier - background dial-back checker for registered pool nodes.
//
// Every iteration (default ~60s), pull up to N peerIds from the pool
// database ordered by least-recently-verified, and for each one ask the
// local IPFS node to swarm-connect to it. Success = the peer is findable
// and reachable via IPFS. Failure = it's not, at least not from here.
//
// Verification is hybrid so NAT'd node operators can still be paid:
//   1. Direct dial (swarm/connect) — strongest proof, works when the
//      operator has forwarded IPFS port 4001 or announced a reachable addr.
//   2. NAT-tolerant fallback — if the dial fails, check whether the peer is
//      in the DHT provider records (findprovs) for a sample of permanent-list
//      CIDs it should be pinning, gated on those CIDs being fetchable at all
//      (bitswap) so stale provider records on a dead network can't pass.
// A node passes if EITHER check succeeds. Port-forwarding is therefore
// recommended (more reliable) but no longer required to earn payouts.
//
// Phase 3 will gate payouts on (verifyFails == 0 && lastVerifyOk recent),
// so a node that silently falls off IPFS won't get paid even if it keeps
// pinging /keepalive.
//

#ifndef DIGIASSET_POOL_VERIFIER_H
#define DIGIASSET_POOL_VERIFIER_H

#include <atomic>
#include <set>
#include <string>
#include <thread>

class PoolDatabase;

class PoolVerifier {
public:
    PoolVerifier(PoolDatabase& db, const std::string& ipfsApiBase);
    ~PoolVerifier();

    PoolVerifier(const PoolVerifier&) = delete;
    PoolVerifier& operator=(const PoolVerifier&) = delete;

    void start();
    void stop();

    // Live counters for the dashboard.
    uint64_t getProbesAttempted() const { return _probesAttempted.load(); }
    uint64_t getProbesSucceeded() const { return _probesSucceeded.load(); }

private:
    PoolDatabase& _db;
    std::string _ipfsApiBase;
    std::atomic<bool> _running{false};
    std::thread _thread;
    std::atomic<uint64_t> _probesAttempted{0};
    std::atomic<uint64_t> _probesSucceeded{0};

    void loop();

    // Direct-dial check (swarm/connect). True only if the peer is reachable.
    bool dialPeer(const std::string& peerIdOrMultiaddr);

    // Full hybrid check: dial first, then fall back to provider-record
    // membership. `providers` is the union of findprovs results for the
    // sampled CIDs this iteration; empty disables the fallback (e.g. when no
    // sample CID was fetchable, so we don't trust the DHT state).
    bool verifyPeer(const std::string& peerIdOrMultiaddr,
                    const std::set<std::string>& providers);

    // IPFS HTTP API helpers for the NAT-tolerant fallback.
    std::set<std::string> findProviders(const std::string& cid);
    bool fetchable(const std::string& cid);
};

#endif // DIGIASSET_POOL_VERIFIER_H
