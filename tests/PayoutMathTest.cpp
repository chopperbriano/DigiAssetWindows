//
// Unit tests for the pool's pure payout math (pool/PayoutMath.h). Included by
// relative path because the pool exe is not part of the test target; the header
// is standalone (only <cstdint>), so it compiles into the tests directly.
//

#include "gtest/gtest.h"
#include "../pool/PayoutMath.h"

// ---- payoutSats: DGB -> satoshis, half-up rounding -------------------------

TEST(PayoutMath, RoundsToNearestSatNotTruncate) {
    EXPECT_EQ(PayoutMath::payoutSats(1.0), 100000000);
    EXPECT_EQ(PayoutMath::payoutSats(1.5), 150000000);
    EXPECT_EQ(PayoutMath::payoutSats(0.00000001), 1);
    EXPECT_EQ(PayoutMath::payoutSats(0.0), 0);
    // A fractional satoshi rounds to nearest rather than truncating (the ledger
    // vs sent-amount drift the fix removed): 1.6 sat -> 2, 1.4 sat -> 1.
    EXPECT_EQ(PayoutMath::payoutSats(0.000000016), 2);
    EXPECT_EQ(PayoutMath::payoutSats(0.000000014), 1);
}

TEST(PayoutMath, NeverNegative) {
    EXPECT_EQ(PayoutMath::payoutSats(-1.0), 0);
}

// ---- isBelowDust: sub-dust shares are dropped from the atomic batch --------

TEST(PayoutMath, DustFloor) {
    EXPECT_TRUE(PayoutMath::isBelowDust(0.00005));        // below 0.0001
    EXPECT_TRUE(PayoutMath::isBelowDust(0.0000000057));   // decayed-weight share that once bricked the batch
    EXPECT_TRUE(PayoutMath::isBelowDust(0.0));
    EXPECT_FALSE(PayoutMath::isBelowDust(0.0001));        // exactly the threshold is payable
    EXPECT_FALSE(PayoutMath::isBelowDust(1.0));
}
