//
// NodeStats.cpp - implementation of the NodeStats singleton (see NodeStats.h).
// Thread-safe mutex-guarded storage shared between the dashboard (writer) and the
// getnodestats RPC method (reader) for cached bitswap and storage-coverage stats.
//

#include "NodeStats.h"

// Meyers singleton: the single instance is created on first call and shared thereafter.
NodeStats& NodeStats::instance() {
    static NodeStats s;
    return s;
}

// Store the latest IPFS bitswap probe result under lock; throughput counters are
// only updated when bitswap is available.
void NodeStats::setBitswap(bool available, uint64_t blocksSent, uint64_t dataSent, double blocksPerMin) {
    std::lock_guard<std::mutex> lk(_m);
    _bitswapProbed = true;
    _bitswapAvailable = available;
    if (available) {
        _blocksSent = blocksSent;
        _dataSent = dataSent;
        _blocksPerMin = blocksPerMin;
    }
}

// Store the latest permanent-storage coverage counts under lock and mark coverage checked.
void NodeStats::setCoverage(unsigned int tracked, unsigned int have) {
    std::lock_guard<std::mutex> lk(_m);
    _coverageChecked = true;
    _coverageTracked = tracked;
    _coverageHave = have;
}

// Copy every cached field into a Snapshot while holding the lock so the reader
// sees a consistent view.
NodeStats::Snapshot NodeStats::snapshot() {
    std::lock_guard<std::mutex> lk(_m);
    Snapshot s;
    s.bitswapProbed = _bitswapProbed;
    s.bitswapAvailable = _bitswapAvailable;
    s.blocksSent = _blocksSent;
    s.dataSent = _dataSent;
    s.blocksPerMin = _blocksPerMin;
    s.coverageChecked = _coverageChecked;
    s.coverageTracked = _coverageTracked;
    s.coverageHave = _coverageHave;
    return s;
}
