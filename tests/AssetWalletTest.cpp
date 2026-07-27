//
// Unit tests for the pure money-parsing helpers in AssetWallet: parseAssetAmount
// (display units -> smallest asset units) and dgbToSats (DGB amount -> satoshis).
// These are exactly the functions whose precision/overflow bugs shipped in an
// earlier build, so they get dedicated coverage.
//

#include "gtest/gtest.h"
#include "AssetWallet.h"
#include <jsoncpp/json/value.h>

using AssetWallet::dgbToSats;
using AssetWallet::parseAssetAmount;
using AssetWallet::satsToDecimal;

// ---- parseAssetAmount ------------------------------------------------------

TEST(ParseAssetAmount, IntegerExact) {
    EXPECT_EQ(parseAssetAmount(Json::Value(5), 0), 5u);
    EXPECT_EQ(parseAssetAmount(Json::Value(5), 2), 500u);
    EXPECT_EQ(parseAssetAmount(Json::Value(1), 8), 100000000u);
}

TEST(ParseAssetAmount, StringDecimal) {
    EXPECT_EQ(parseAssetAmount(Json::Value("1.5"), 2), 150u);
    EXPECT_EQ(parseAssetAmount(Json::Value("0.01"), 2), 1u);
    EXPECT_EQ(parseAssetAmount(Json::Value("1234.5678"), 4), 12345678u);
}

// Regression: a 7-decimal amount passed as a JSON NUMBER must not be rounded to
// 6 decimals. The old to_string(double) path turned 0.0000015 into "0.000002"
// (20 units) - a 33% over-send.
TEST(ParseAssetAmount, SevenDecimalNumberNotTruncated) {
    EXPECT_EQ(parseAssetAmount(Json::Value(0.0000015), 7), 15u);
    EXPECT_EQ(parseAssetAmount(Json::Value(1.2345678), 7), 12345678u);
}

TEST(ParseAssetAmount, SevenDecimalStringExact) {
    EXPECT_EQ(parseAssetAmount(Json::Value("0.0000015"), 7), 15u);
}

TEST(ParseAssetAmount, TooManyDecimalsThrows) {
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value("0.001"), 2)); // 3 decimals on a 2-decimal asset
}

TEST(ParseAssetAmount, ZeroOrNegativeThrows) {
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value(0), 2));
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value(-1), 2));
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value("0"), 2));
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value("0.00"), 2));
}

TEST(ParseAssetAmount, InvalidStringThrows) {
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value("abc"), 2));
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value("1.2.3"), 2));
}

// Regression: an out-of-range whole part must be rejected BEFORE the *10^decimals
// multiply. 1844674407371 * 10^7 overflows uint64 and would wrap to 448384 (a
// valid-looking small count) without the pre-multiply guard.
TEST(ParseAssetAmount, OverflowRejectedNotWrapped) {
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value("1844674407371"), 7));
    EXPECT_ANY_THROW(parseAssetAmount(Json::Value(static_cast<Json::Int64>(1844674407371LL)), 7));
}

// ---- dgbToSats (used by sendmany's same-address merge) ---------------------

TEST(DgbToSats, NumberAndString) {
    EXPECT_EQ(dgbToSats(Json::Value("1.0")), 100000000);
    EXPECT_EQ(dgbToSats(Json::Value(0.1)), 10000000);
    EXPECT_EQ(dgbToSats(Json::Value("0.00000001")), 1);
    EXPECT_EQ(dgbToSats(Json::Value(5)), 500000000);
}

// The merge regression: 0.1 + 0.2 summed as doubles = 0.30000000000000004, which
// Core rejects. Summing in sats is exact and re-serializes to a valid amount.
TEST(DgbToSats, ExactMergeAvoidsFloatArtifact) {
    int64_t merged = dgbToSats(Json::Value(0.1)) + dgbToSats(Json::Value(0.2));
    EXPECT_EQ(merged, 30000000);
    EXPECT_EQ(satsToDecimal(static_cast<uint64_t>(merged)), "0.30000000");
}

TEST(DgbToSats, RejectsBadAmounts) {
    EXPECT_ANY_THROW(dgbToSats(Json::Value("-1.0")));       // negative
    EXPECT_ANY_THROW(dgbToSats(Json::Value("1.234567891"))); // 9 decimals
    EXPECT_ANY_THROW(dgbToSats(Json::Value("abc")));
    EXPECT_ANY_THROW(dgbToSats(Json::Value(Json::arrayValue)));
}
