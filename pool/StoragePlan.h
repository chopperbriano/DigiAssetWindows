//
// StoragePlan - who the pool pays for storage, and how much each is owed.
//
// Header-only and dependency-free (std only) like PayoutMath.h, so the rules
// are unit-tested directly (tests/StoragePlanTest.cpp) without linking the
// pool executable.
//
// THE PROBLEM THIS SOLVES. The pool paid every verified node a share of one
// pot, weighted by how much of the WHOLE list it seemed to hold. Every node
// stores everything, so each new node only shrank everyone's share: a pot
// split twenty ways is rounding errors twenty times. And the weight counted
// files, not bytes, so a node holding one large video earned the same as one
// holding a thumbnail.
//
// THE PLAN, each payout period:
//
//   1. A capped PAID SET. Eligible nodes ranked by reliability, then by how
//      long they have served (firstSeen), then peerId for a stable order; the
//      top `maxPaidNodes` (default 10) are paid. 0 means no cap. The rest wait their turn
//      and keep their scores, so a paid node that drops out is replaced.
//
//   2. A fixed number of PAID COPIES per file. Each CID is assigned to
//      `replicas` nodes of the paid set by rendezvous hashing (highest
//      hash(cid|peerId) wins), so assignments are stable: adding or losing a
//      node moves only the files that node held, not the whole list.
//
//   3. PAID BY BYTES. A node's weight is the bytes it is assigned, times how
//      surely it holds them (`held`), times its reliability. A CID whose size
//      the pool has not measured yet counts `unknownSizeBytes`.
//
//   4. `held` is the node's proof-of-storage score when it has ever answered a
//      byte-range challenge (proofAnswered > 0). A node on an older client that
//      cannot answer falls back to the DHT coverage estimate, scaled by
//      `legacyFactor`, so it still earns but less than a node that proves.
//
// The pot itself is unchanged: the budget is still a share of the pool wallet,
// split by these weights. This only decides who it goes to.
//

#ifndef DIGIASSET_POOL_STORAGEPLAN_H
#define DIGIASSET_POOL_STORAGEPLAN_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace StoragePlan {

    struct Config {
        unsigned int replicas = 3;          // paid copies per CID
        unsigned int maxPaidNodes = 10;     // 0 = no cap.  Capped by default: uncapped, every new
                                            // node shrinks every paid node's share

        double legacyFactor = 0.5;          // weight kept by a node that never proves
        int64_t unknownSizeBytes = 1;       // weight of a CID not yet measured
    };

    struct Node {
        std::string peerId;
        double reliability = 0.0;   // 0..1 EMA of verify rounds passed
        double coverage = 0.0;      // 0..1 EMA, DHT provider estimate
        double proofScore = 0.0;    // 0..1 EMA of storage challenges passed
        int64_t proofAnswered = 0;  // challenges ever answered, right or wrong
        int64_t firstSeen = 0;
    };

    struct Cid {
        std::string cid;
        int64_t sizeBytes = 0;      // 0 = not measured yet
    };

    struct Weight {
        std::string peerId;
        int64_t assignedBytes = 0;
        unsigned int assignedCids = 0;
        double held = 0.0;          // 0..1, how surely it holds what it is assigned
        bool legacy = false;        // judged by DHT coverage, not by proofs
        double weight = 0.0;        // assignedBytes * held * reliability
    };

    // FNV-1a 64-bit. Stable across builds and platforms, which a payout
    // assignment must be: std::hash is not guaranteed to be.
    inline uint64_t fnv1a(const std::string& s) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c: s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    // How surely a node holds what it is assigned, 0..1.
    inline double heldScore(const Node& n, const Config& cfg) {
        double f = cfg.legacyFactor < 0.0 ? 0.0 : (cfg.legacyFactor > 1.0 ? 1.0 : cfg.legacyFactor);
        return n.proofAnswered > 0 ? n.proofScore : n.coverage * f;
    }

    // The nodes paid this period, in rank order. Only nodes with something to
    // show (held > 0 and reliability > 0) can take a paid slot, so a node that
    // holds nothing cannot crowd out one that does.
    inline std::vector<Node> paidSet(std::vector<Node> nodes, const Config& cfg) {
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& n) {
                        return n.reliability <= 0.0 || heldScore(n, cfg) <= 0.0;
                    }),
                    nodes.end());
        std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) {
            if (a.reliability != b.reliability) return a.reliability > b.reliability;
            if (a.firstSeen != b.firstSeen) return a.firstSeen < b.firstSeen;
            return a.peerId < b.peerId;
        });
        if (cfg.maxPaidNodes > 0 && nodes.size() > cfg.maxPaidNodes) nodes.resize(cfg.maxPaidNodes);
        return nodes;
    }

    // The peerIds a CID is assigned to: the `replicas` paid nodes with the
    // highest rendezvous hash for it.
    inline std::vector<std::string> assignees(const std::string& cid, const std::vector<Node>& paid,
                                              unsigned int replicas) {
        std::vector<std::pair<uint64_t, std::string>> ranked;
        ranked.reserve(paid.size());
        for (const auto& n: paid) ranked.emplace_back(fnv1a(cid + "|" + n.peerId), n.peerId);
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
        std::vector<std::string> out;
        for (size_t i = 0; i < ranked.size() && i < replicas; i++) out.push_back(ranked[i].second);
        return out;
    }

    // Every paid node's weight. Nodes outside the paid set do not appear.
    inline std::vector<Weight> weights(const std::vector<Node>& nodes, const std::vector<Cid>& cids,
                                       const Config& cfg) {
        auto paid = paidSet(nodes, cfg);
        unsigned int replicas = cfg.replicas == 0 ? 1 : cfg.replicas;
        std::map<std::string, Weight> byPeer;
        for (const auto& n: paid) {
            Weight w;
            w.peerId = n.peerId;
            w.held = heldScore(n, cfg);
            w.legacy = n.proofAnswered == 0;
            byPeer[n.peerId] = w;
        }
        for (const auto& c: cids) {
            int64_t bytes = c.sizeBytes > 0 ? c.sizeBytes : cfg.unknownSizeBytes;
            for (const auto& peer: assignees(c.cid, paid, replicas)) {
                byPeer[peer].assignedBytes += bytes;
                byPeer[peer].assignedCids += 1;
            }
        }
        std::vector<Weight> out;
        for (const auto& n: paid) {
            Weight w = byPeer[n.peerId];
            w.weight = (double) w.assignedBytes * w.held * n.reliability;
            out.push_back(w);
        }
        return out;
    }

    // Is this CID one of the node's assignments? The challenger prefers these,
    // because proving a file nobody pays you for proves the wrong thing.
    inline bool isAssigned(const std::string& cid, const std::string& peerId,
                           const std::vector<Node>& paid, unsigned int replicas) {
        for (const auto& p: assignees(cid, paid, replicas == 0 ? 1 : replicas)) {
            if (p == peerId) return true;
        }
        return false;
    }

    // ---- Storage challenges ----------------------------------------------
    //
    // The pool asks a node for the SHA-256 of a byte range of a file it should
    // hold, and checks the answer against its own copy. The node reads only its
    // own pinned data, so a node that stores nothing has to fetch the file from
    // the network inside the deadline to answer, which costs it the bandwidth
    // the pool is paying to avoid. It is not a cryptographic proof against a
    // determined cheat with a fast pipe; it makes cheating cost about what
    // honest storage costs.

    constexpr int64_t CHALLENGE_MAX_BYTES = 64 * 1024;
    constexpr int64_t CHALLENGE_DEADLINE_SECONDS = 120;
    constexpr double PROOF_EMA = 0.2;

    struct Range {
        int64_t offset = 0;
        int64_t length = 0;
    };

    // A byte range within a file of `sizeBytes`, chosen from `seed`. A file
    // smaller than the range is challenged whole.
    inline Range challengeRange(int64_t sizeBytes, uint64_t seed) {
        Range r;
        if (sizeBytes <= 0) return r;
        r.length = std::min<int64_t>(sizeBytes, CHALLENGE_MAX_BYTES);
        int64_t span = sizeBytes - r.length;
        r.offset = span > 0 ? (int64_t) (seed % (uint64_t) (span + 1)) : 0;
        return r;
    }

    // Fold one challenge result into a node's proof score.
    inline double nextProofScore(double current, bool passed) {
        return PROOF_EMA * (passed ? 1.0 : 0.0) + (1.0 - PROOF_EMA) * current;
    }

    // Lowercase, trimmed hex comparison: an answer differs from the expected
    // hash only by case or whitespace is still the same answer.
    inline bool sameHash(std::string a, std::string b) {
        auto norm = [](std::string& s) {
            s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }),
                    s.end());
            for (auto& c: s) c = (char) std::tolower((unsigned char) c);
        };
        norm(a);
        norm(b);
        return a.size() == 64 && a == b;
    }

    // Read the plan's knobs from pool.cfg, keeping the defaults on anything
    // missing or malformed.
    inline Config configFrom(const std::map<std::string, std::string>& cfg) {
        Config c;
        auto num = [&](const char* key, auto& out) {
            auto it = cfg.find(key);
            if (it == cfg.end()) return;
            try {
                using T = std::decay_t<decltype(out)>;
                if constexpr (std::is_floating_point_v<T>) out = std::stod(it->second);
                else out = (T) std::stoll(it->second);
            } catch (...) {}
        };
        num("poolreplicas", c.replicas);
        num("poolmaxpaidnodes", c.maxPaidNodes);
        num("poollegacyweight", c.legacyFactor);
        return c;
    }

} // namespace StoragePlan

#endif // DIGIASSET_POOL_STORAGEPLAN_H
