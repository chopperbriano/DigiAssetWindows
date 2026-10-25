// PoolLog.h - the pool server's log file.
//
// The dashboard keeps its log lines in memory only, and the few messages written straight to
// stderr (a rejected registration, a payout sent but not ledgered) are painted over by the next
// dashboard redraw.  Every one of them also goes to pool.log (poollogpath in pool.cfg), rotated
// to pool.log.1 once it reaches 50 MB - the same writer the node uses for ipfs-failures.log.
//
// Declarations only: Log.h brings `using namespace std`, which clashes with windows.h's byte in
// the files that include this, so only PoolLog.cpp includes it.

#ifndef DIGIASSET_POOL_POOLLOG_H
#define DIGIASSET_POOL_POOLLOG_H

#include <string>

namespace PoolLog {
    // Where lines go; set from pool.cfg at startup.  Empty turns the file off.
    void setPath(const std::string& path);

    // Appends one timestamped line.  Never throws: a log we cannot write must not stop the pool.
    void line(const std::string& text);
}

#endif //DIGIASSET_POOL_POOLLOG_H
