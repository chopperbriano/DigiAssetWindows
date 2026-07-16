//
// Tests for RPC::Response — no external dependencies required.
//

#include "RPC/Response.h"
#include "gtest/gtest.h"

#include <jsoncpp/json/value.h>
#include <string>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// empty()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Response, empty_defaultConstructed) {
    RPC::Response r;
    EXPECT_TRUE(r.empty());
}

TEST(RPC_Response, empty_afterSetResult) {
    RPC::Response r;
    r.setResult(Json::Value("hello"));
    EXPECT_FALSE(r.empty());
}

TEST(RPC_Response, empty_afterSetError) {
    RPC::Response r;
    r.setError(Json::Value("error message"));
    EXPECT_FALSE(r.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// size()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Response, size_growsAfterSetResult) {
    RPC::Response r;
    size_t before = r.size();
    r.setResult(Json::Value("some data"));
    EXPECT_GT(r.size(), before);
}

TEST(RPC_Response, size_growsAfterAddInvalidateOnAddressChange) {
    RPC::Response r;
    size_t before = r.size();
    r.addInvalidateOnAddressChange("dgb1qtest");
    EXPECT_GT(r.size(), before);
}

TEST(RPC_Response, size_duplicateAddressNotAdded) {
    RPC::Response r;
    r.addInvalidateOnAddressChange("dgb1qtest");
    size_t after_first = r.size();
    r.addInvalidateOnAddressChange("dgb1qtest"); // duplicate
    EXPECT_EQ(r.size(), after_first);
}

// ─────────────────────────────────────────────────────────────────────────────
// addressChanged()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Response, addressChanged_watchedAddress_returnsSize) {
    RPC::Response r;
    r.setResult(Json::Value(42));
    r.addInvalidateOnAddressChange("dgb1qwatched");
    size_t result = r.addressChanged("dgb1qwatched");
    EXPECT_GT(result, 0u);
}

TEST(RPC_Response, addressChanged_unwatchedAddress_returnsZero) {
    RPC::Response r;
    r.setResult(Json::Value(42));
    r.addInvalidateOnAddressChange("dgb1qwatched");
    EXPECT_EQ(r.addressChanged("dgb1qother"), 0u);
}

TEST(RPC_Response, addressChanged_noWatchedAddresses_returnsZero) {
    RPC::Response r;
    r.setResult(Json::Value(true));
    EXPECT_EQ(r.addressChanged("dgb1qanything"), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// newBlockAdded()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Response, newBlockAdded_neverExpires_whenNotSet) {
    RPC::Response r;
    r.setResult(Json::Value("data"));
    // Default _blocksGoodFor = 0, first call → drops to -1 → returns size
    // Actually: newBlockAdded decrements then checks <0
    size_t result = r.newBlockAdded();
    EXPECT_GT(result, 0u) << "Default blocksGoodFor=0 → expires after first block";
}

TEST(RPC_Response, newBlockAdded_expiresAfterSetBlocks) {
    RPC::Response r;
    r.setResult(Json::Value(1));
    r.setBlocksGoodFor(2);

    EXPECT_EQ(r.newBlockAdded(), 0u) << "block 1: still good (blocksGoodFor=1)";
    EXPECT_EQ(r.newBlockAdded(), 0u) << "block 2: still good (blocksGoodFor=0)";
    EXPECT_GT(r.newBlockAdded(), 0u) << "block 3: expired (blocksGoodFor=-1)";
}

TEST(RPC_Response, newBlockAdded_negativeBlocksGoodFor_neverCaches) {
    RPC::Response r;
    r.setResult(Json::Value(true));
    r.setBlocksGoodFor(-1);
    // First call: -1 - 1 = -2 < 0 → should invalidate immediately
    EXPECT_GT(r.newBlockAdded(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// newAssetIssued()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Response, newAssetIssued_notSet_returnsZero) {
    RPC::Response r;
    r.setResult(Json::Value("data"));
    EXPECT_EQ(r.newAssetIssued(), 0u);
}

TEST(RPC_Response, newAssetIssued_set_returnsSize) {
    RPC::Response r;
    r.setResult(Json::Value("data"));
    r.setInvalidateOnNewAsset();
    EXPECT_GT(r.newAssetIssued(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// toJSON()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Response, toJSON_resultResponse) {
    RPC::Response r;
    r.setResult(Json::Value(42));
    Json::Value json = r.toJSON(Json::Value(1));
    EXPECT_FALSE(json["result"].isNull());
    EXPECT_TRUE(json["error"].isNull());
    EXPECT_EQ(json["id"].asInt(), 1);
    EXPECT_EQ(json["result"].asInt(), 42);
}

TEST(RPC_Response, toJSON_errorResponse) {
    RPC::Response r;
    r.setError(Json::Value("something went wrong"));
    Json::Value json = r.toJSON(Json::Value(99));
    EXPECT_TRUE(json["result"].isNull());
    EXPECT_FALSE(json["error"].isNull());
    EXPECT_EQ(json["id"].asInt(), 99);
    EXPECT_EQ(json["error"].asString(), "something went wrong");
}

TEST(RPC_Response, toJSON_hasIdField) {
    RPC::Response r;
    r.setResult(Json::Value(true));
    Json::Value json = r.toJSON(Json::Value("req-1"));
    EXPECT_EQ(json["id"].asString(), "req-1");
}
