//
// NodeStats - shared process-wide cache of slow-to-compute stats so that
// the dashboard and the RPC method getnodestats can both report the same
// numbers without either one duplicating the HTTP calls or holding up the
// RPC thread on slow network fetches.
//
// The dashboard is the writer: pollBitswapStats() and checkPermanentCoverage()
// in ConsoleDashboard.cpp populate these fields as they complete. The RPC
// method getnodestats.cpp is the reader. All accessors are mutex-guarded.
//

#ifndef DIGIASSET_CORE_NODESTATS_H
#define DIGIASSET_CORE_NODESTATS_H

#include <cstdint>
#include <mutex>
#include <string>

// Process-wide singleton cache of expensive-to-compute node stats (IPFS bitswap
// activity and permanent-storage coverage). The dashboard writes via the setters
// as its background probes complete; readers take a consistent copy via snapshot().
class NodeStats {
    std::mutex _m;
    bool _bitswapProbed = false;
    bool _bitswapAvailable = false;
    uint64_t _blocksSent = 0;
    uint64_t _dataSent = 0;
    double _blocksPerMin = 0.0;

    bool _coverageChecked = false;
    unsigned int _coverageTracked = 0;
    unsigned int _coverageHave = 0;

    // Pool + payout, populated by the dashboard's background checks so the web
    // console and getnodestats can report them without doing their own fetches.
    bool _poolProbed = false;
    bool _poolReachable = false;
    unsigned int _poolNodesOnline = 0;
    std::string _poolServer;
    std::string _poolStatus;     // human text, e.g. "Hosting pool files"
    std::string _paymentStatus;  // human text, e.g. "active" / "registered (no payouts yet)"
    std::string _payoutAddress;
    std::string _payoutBalance;

    NodeStats() = default;

public:
    NodeStats(const NodeStats&) = delete;
    NodeStats& operator=(const NodeStats&) = delete;
    // Accessor for the single shared instance.
    static NodeStats& instance();

    // Record latest IPFS bitswap probe results; marks bitswap as probed. When
    // available is false the throughput counters are left unchanged.
    void setBitswap(bool available, uint64_t blocksSent, uint64_t dataSent, double blocksPerMin);
    // Record how many tracked assets the node has content for; marks coverage as checked.
    void setCoverage(unsigned int tracked, unsigned int have);

    // Record the configured pool's reachability + online node count; marks pool probed.
    void setPool(bool reachable, unsigned int nodesOnline, const std::string& server);
    // Record the human-readable pool hosting + payment status strings.
    void setPoolStatus(const std::string& poolStatus, const std::string& paymentStatus);
    // Record the payout address + last-known balance string.
    void setPayout(const std::string& address, const std::string& balance);

    // Plain-data copy of all cached fields, returned atomically by snapshot().
    struct Snapshot {
        bool bitswapProbed;
        bool bitswapAvailable;
        uint64_t blocksSent;
        uint64_t dataSent;
        double blocksPerMin;
        bool coverageChecked;
        unsigned int coverageTracked;
        unsigned int coverageHave;
        bool poolProbed;
        bool poolReachable;
        unsigned int poolNodesOnline;
        std::string poolServer;
        std::string poolStatus;
        std::string paymentStatus;
        std::string payoutAddress;
        std::string payoutBalance;
    };
    // Return a lock-guarded consistent copy of all current stats.
    Snapshot snapshot();
};

#endif // DIGIASSET_CORE_NODESTATS_H
