//
// Pure payout money math for the pool, header-only so it can be unit-tested
// without linking the pool executable (the pool is not part of the test target).
// Kept tiny and dependency-free on purpose - this is exactly the kind of code
// whose rounding/threshold bugs are cheap to test and expensive to ship.
//

#ifndef DIGIASSET_POOL_PAYOUTMATH_H
#define DIGIASSET_POOL_PAYOUTMATH_H

#include <cstdint>

namespace PayoutMath {

    // Network dust threshold in DGB. A payout below this can't be sent (Core
    // rejects the output), and because the payout is one atomic sendmany a single
    // sub-dust output would strand EVERY recipient - so such a recipient is
    // skipped instead. 0.0001 DGB = 10000 sats = DIGIBYTE_DUST.
    constexpr double DUST_DGB = 0.0001;

    // Convert a DGB amount to whole satoshis with consistent half-up rounding.
    // Used for BOTH the sendmany amount and the ledger record so the value sent
    // and the value recorded can never disagree (was: %.8f round vs int64 trunc).
    inline int64_t payoutSats(double dgb) {
        if (dgb < 0.0) dgb = 0.0;
        return (int64_t) (dgb * 100000000.0 + 0.5);
    }

    // True if a payout share is too small to send (below the dust threshold) and
    // must be dropped from the batch.
    inline bool isBelowDust(double dgb) {
        return dgb < DUST_DGB;
    }

} // namespace PayoutMath

#endif // DIGIASSET_POOL_PAYOUTMATH_H
