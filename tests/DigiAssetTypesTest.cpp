//
// Tests for DigiAssetTypes structs — no external dependencies required.
//

#include "DigiAssetTypes.h"
#include "serialize.h"
#include "gtest/gtest.h"

#include <cstdint>
#include <vector>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Signer
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiAssetTypes, Signer_equalityOperator) {
    Signer a{"addr1", 10};
    Signer b{"addr1", 10};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(DigiAssetTypes, Signer_inequality_differentAddress) {
    Signer a{"addr1", 10};
    Signer b{"addr2", 10};
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(DigiAssetTypes, Signer_inequality_differentWeight) {
    Signer a{"addr1", 10};
    Signer b{"addr1", 99};
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(DigiAssetTypes, Signer_serializeDeserialize_roundTrip) {
    Signer original{"dgb1qexample", 42ULL};

    vector<uint8_t> data;
    serialize(data, original);

    size_t i = 0;
    Signer result;
    deserialize(data, i, result);

    EXPECT_EQ(result.address, original.address);
    EXPECT_EQ(result.weight, original.weight);
    EXPECT_EQ(result, original);
}

TEST(DigiAssetTypes, Signer_serializeDeserialize_emptyAddress) {
    Signer original{"", 0ULL};

    vector<uint8_t> data;
    serialize(data, original);

    size_t i = 0;
    Signer result;
    deserialize(data, i, result);

    EXPECT_EQ(result.address, "");
    EXPECT_EQ(result.weight, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ExchangeRate
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiAssetTypes, ExchangeRate_enabled_nonEmptyAddress) {
    ExchangeRate er{"dgb1qaddr", 0, "USD"};
    EXPECT_TRUE(er.enabled());
}

TEST(DigiAssetTypes, ExchangeRate_disabled_emptyAddress) {
    ExchangeRate er{"", 0, ""};
    EXPECT_FALSE(er.enabled());
}

TEST(DigiAssetTypes, ExchangeRate_equality_bothEnabled) {
    ExchangeRate a{"dgb1q", 1, "USD"};
    ExchangeRate b{"dgb1q", 1, "EUR"}; // name not tested in comparison
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(DigiAssetTypes, ExchangeRate_equality_differentIndex) {
    ExchangeRate a{"dgb1q", 1, "USD"};
    ExchangeRate b{"dgb1q", 2, "USD"};
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(DigiAssetTypes, ExchangeRate_equality_bothDisabled) {
    ExchangeRate a{"", 0, ""};
    ExchangeRate b{"", 1, "foo"}; // both empty address → equal
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(DigiAssetTypes, ExchangeRate_equality_oneDisabled) {
    ExchangeRate a{"dgb1q", 0, ""};
    ExchangeRate b{"", 0, ""};
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(DigiAssetTypes, ExchangeRate_serializeDeserialize_roundTrip) {
    ExchangeRate original{"dgb1qtest", 3, "BTC"};

    vector<uint8_t> data;
    serialize(data, original);

    size_t i = 0;
    ExchangeRate result;
    deserialize(data, i, result);

    EXPECT_EQ(result.address, original.address);
    EXPECT_EQ(result.index, original.index);
    EXPECT_EQ(result.name, original.name);
}

TEST(DigiAssetTypes, ExchangeRate_serializeDeserialize_disabled) {
    ExchangeRate original{"", 0, ""};

    vector<uint8_t> data;
    serialize(data, original);

    size_t i = 0;
    ExchangeRate result;
    result.address = "notempty"; // ensure it gets overwritten
    deserialize(data, i, result);

    EXPECT_FALSE(result.enabled());
}

// ─────────────────────────────────────────────────────────────────────────────
// Royalty
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiAssetTypes, Royalty_equalityOperator) {
    Royalty a{"addr1", 100};
    Royalty b{"addr1", 100};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(DigiAssetTypes, Royalty_inequality_differentAmount) {
    Royalty a{"addr1", 100};
    Royalty b{"addr1", 200};
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(DigiAssetTypes, Royalty_inequality_differentAddress) {
    Royalty a{"addr1", 100};
    Royalty b{"addr2", 100};
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(DigiAssetTypes, Royalty_serializeDeserialize_roundTrip) {
    Royalty original{"dgb1qroyalty", 9999999ULL};

    vector<uint8_t> data;
    serialize(data, original);

    size_t i = 0;
    Royalty result;
    deserialize(data, i, result);

    EXPECT_EQ(result.address, original.address);
    EXPECT_EQ(result.amount, original.amount);
    EXPECT_EQ(result, original);
}

// ─────────────────────────────────────────────────────────────────────────────
// VoteOption
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiAssetTypes, VoteOption_equality_sameAddress) {
    VoteOption a{"addr1", "Yes"};
    VoteOption b{"addr1", "No"}; // label not tested in comparison
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(DigiAssetTypes, VoteOption_inequality_differentAddress) {
    VoteOption a{"addr1", "Yes"};
    VoteOption b{"addr2", "Yes"};
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(DigiAssetTypes, VoteOption_serializeDeserialize_roundTrip) {
    VoteOption original{"dgb1qvote", "Option A"};

    vector<uint8_t> data;
    serialize(data, original);

    size_t i = 0;
    VoteOption result;
    deserialize(data, i, result);

    EXPECT_EQ(result.address, original.address);
    EXPECT_EQ(result.label, original.label);
    EXPECT_EQ(result, original);
}

TEST(DigiAssetTypes, VoteOption_serializeDeserialize_emptyLabel) {
    VoteOption original{"dgb1q", ""};

    vector<uint8_t> data;
    serialize(data, original);

    size_t i = 0;
    VoteOption result;
    deserialize(data, i, result);

    EXPECT_EQ(result.address, "dgb1q");
    EXPECT_EQ(result.label, "");
}
