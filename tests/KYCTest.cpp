//
// Tests for KYC class — no external dependencies required.
//

#include "KYC.h"
#include "gtest/gtest.h"

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Default constructor
// ─────────────────────────────────────────────────────────────────────────────

TEST(KYC, defaultConstructor_empty) {
    KYC kyc;
    EXPECT_TRUE(kyc.empty());
    EXPECT_FALSE(kyc.valid());
    EXPECT_EQ(kyc.getAddress(), "");
    EXPECT_EQ(kyc.getHeightRevoked(), -1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Address-only constructor
// ─────────────────────────────────────────────────────────────────────────────

TEST(KYC, addressOnlyConstructor) {
    KYC kyc("dgb1qtest");
    EXPECT_EQ(kyc.getAddress(), "dgb1qtest");
    EXPECT_TRUE(kyc.empty()); // heightCreated = -1 (not yet verified)
    EXPECT_FALSE(kyc.valid());

    // Getters that require heightCreated != -1 should throw
    EXPECT_THROW(kyc.getName(), KYC::exceptionUnknownValue);
    EXPECT_THROW(kyc.getHash(), KYC::exceptionUnknownValue);
    EXPECT_THROW(kyc.getCountry(), KYC::exceptionUnknownValue);
    EXPECT_THROW(kyc.getHeightCreated(), KYC::exceptionUnknownValue);
}

// ─────────────────────────────────────────────────────────────────────────────
// Full constructor — verified, never revoked
// ─────────────────────────────────────────────────────────────────────────────

TEST(KYC, fullConstructor_getters) {
    KYC kyc("dgb1qtest", "USA", "Alice Smith", "abc123hash", 1000000);

    EXPECT_FALSE(kyc.empty());
    EXPECT_EQ(kyc.getAddress(), "dgb1qtest");
    EXPECT_EQ(kyc.getCountry(), "USA");
    EXPECT_EQ(kyc.getName(), "Alice Smith");
    EXPECT_EQ(kyc.getHash(), "abc123hash");
    EXPECT_EQ(kyc.getHeightCreated(), 1000000u);
    EXPECT_EQ(kyc.getHeightRevoked(), -1); // never revoked
}

// ─────────────────────────────────────────────────────────────────────────────
// valid() — height-aware logic
// ─────────────────────────────────────────────────────────────────────────────

TEST(KYC, valid_neverRevoked) {
    KYC kyc("dgb1qtest", "USA", "Alice", "", 1000, -1);

    // valid() with -1 → uses "highest scanned"; never revoked → true
    EXPECT_TRUE(kyc.valid(-1));
    EXPECT_TRUE(kyc.valid());

    // valid at or after creation height
    EXPECT_TRUE(kyc.valid(1000));
    EXPECT_TRUE(kyc.valid(99999));

    // before creation height
    EXPECT_FALSE(kyc.valid(999));
    EXPECT_FALSE(kyc.valid(0));
}

TEST(KYC, valid_withRevoke) {
    // Created at 1000, revoked at 2000
    KYC kyc("dgb1qtest", "USA", "Bob", "", 1000, 2000);

    // valid at or before revoke
    EXPECT_TRUE(kyc.valid(1000));
    EXPECT_TRUE(kyc.valid(1999));

    // invalid at or after revoke
    EXPECT_FALSE(kyc.valid(2000));
    EXPECT_FALSE(kyc.valid(9999));

    // valid(-1) → "highest scanned" → was revoked at some point → false
    EXPECT_FALSE(kyc.valid(-1));
    EXPECT_FALSE(kyc.valid());
}

TEST(KYC, valid_beforeCreation) {
    KYC kyc("dgb1qtest", "CAD", "Charlie", "", 500);
    EXPECT_FALSE(kyc.valid(0));
    EXPECT_FALSE(kyc.valid(499));
    EXPECT_TRUE(kyc.valid(500));
}

// ─────────────────────────────────────────────────────────────────────────────
// getHeightRevoked() accessor
// ─────────────────────────────────────────────────────────────────────────────

TEST(KYC, getHeightRevoked) {
    KYC never("a", "USA", "X", "", 100, -1);
    EXPECT_EQ(never.getHeightRevoked(), -1);

    KYC revoked("b", "USA", "Y", "", 100, 500);
    EXPECT_EQ(revoked.getHeightRevoked(), 500);
}

// ─────────────────────────────────────────────────────────────────────────────
// empty() — depends only on heightCreated
// ─────────────────────────────────────────────────────────────────────────────

TEST(KYC, empty_vs_nonEmpty) {
    KYC empty;
    EXPECT_TRUE(empty.empty());

    KYC nonEmpty("addr", "USA", "Name", "hash", 100);
    EXPECT_FALSE(nonEmpty.empty());
}
