//
// Tests for RPC::Cache — no external dependencies required.
//

#include "RPC/Cache.h"
#include "RPC/Response.h"
#include "gtest/gtest.h"

#include <jsoncpp/json/value.h>
#include <string>

using namespace std;

static RPC::Response makeResponse(const Json::Value& val, int blocksGoodFor = 10) {
    RPC::Response r;
    r.setResult(val);
    r.setBlocksGoodFor(blocksGoodFor);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// isCached / addResponse
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Cache, isCached_missOnEmpty) {
    RPC::Cache cache;
    RPC::Response out;
    EXPECT_FALSE(cache.isCached("version", Json::Value(Json::arrayValue), out));
}

TEST(RPC_Cache, addResponse_thenIsCached) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;
    auto resp = makeResponse(Json::Value(42));
    cache.addResponse("mymethod", params, resp);

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("mymethod", params, out));
}

TEST(RPC_Cache, isCached_differentMethod_misses) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;
    cache.addResponse("method_a", params, makeResponse(Json::Value(1)));

    RPC::Response out;
    EXPECT_FALSE(cache.isCached("method_b", params, out));
}

TEST(RPC_Cache, isCached_differentParams_misses) {
    RPC::Cache cache;
    Json::Value params1 = Json::arrayValue;
    params1.append(1);
    Json::Value params2 = Json::arrayValue;
    params2.append(2);

    cache.addResponse("method", params1, makeResponse(Json::Value("a")));

    RPC::Response out;
    EXPECT_FALSE(cache.isCached("method", params2, out));
}

TEST(RPC_Cache, addResponse_sameKey_updatesEntry) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;

    cache.addResponse("m", params, makeResponse(Json::Value(1)));
    cache.addResponse("m", params, makeResponse(Json::Value(2)));

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("m", params, out));
    EXPECT_EQ(out.toJSON(1)["result"].asInt(), 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// addressChanged()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Cache, addressChanged_evictsWatchedEntry) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;

    RPC::Response resp;
    resp.setResult(Json::Value("data"));
    resp.setBlocksGoodFor(100);
    resp.addInvalidateOnAddressChange("dgb1qwatched");

    cache.addResponse("getaddress", params, resp);

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("getaddress", params, out));

    cache.addressChanged("dgb1qwatched");

    EXPECT_FALSE(cache.isCached("getaddress", params, out))
        << "Entry should be evicted after watched address changes";
}

TEST(RPC_Cache, addressChanged_doesNotEvictUnwatched) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;

    RPC::Response resp;
    resp.setResult(Json::Value("data"));
    resp.setBlocksGoodFor(100);
    resp.addInvalidateOnAddressChange("dgb1qwatched");

    cache.addResponse("getaddress", params, resp);
    cache.addressChanged("dgb1qother"); // different address

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("getaddress", params, out))
        << "Entry should survive address change for a different address";
}

// ─────────────────────────────────────────────────────────────────────────────
// newBlockAdded()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Cache, newBlockAdded_evictsExpiredEntry) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;

    RPC::Response resp;
    resp.setResult(Json::Value("block_data"));
    resp.setBlocksGoodFor(1);
    cache.addResponse("getblock", params, resp);

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("getblock", params, out));

    cache.newBlockAdded(); // blocksGoodFor → 0
    EXPECT_TRUE(cache.isCached("getblock", params, out)); // still good

    cache.newBlockAdded(); // blocksGoodFor → -1 → evict
    EXPECT_FALSE(cache.isCached("getblock", params, out));
}

TEST(RPC_Cache, newBlockAdded_doesNotEvictLongLivedEntry) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;

    RPC::Response resp;
    resp.setResult(Json::Value("stable"));
    resp.setBlocksGoodFor(100);
    cache.addResponse("stable", params, resp);

    cache.newBlockAdded();
    cache.newBlockAdded();

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("stable", params, out));
}

// ─────────────────────────────────────────────────────────────────────────────
// newAssetIssued()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_Cache, newAssetIssued_evictsAssetSensitiveEntry) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;

    RPC::Response resp;
    resp.setResult(Json::Value("assets"));
    resp.setBlocksGoodFor(100);
    resp.setInvalidateOnNewAsset();
    cache.addResponse("listassets", params, resp);

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("listassets", params, out));

    cache.newAssetIssued();
    EXPECT_FALSE(cache.isCached("listassets", params, out));
}

TEST(RPC_Cache, newAssetIssued_doesNotEvictNonAssetEntry) {
    RPC::Cache cache;
    Json::Value params = Json::arrayValue;

    RPC::Response resp;
    resp.setResult(Json::Value("block info"));
    resp.setBlocksGoodFor(100);
    // NOT calling setInvalidateOnNewAsset()
    cache.addResponse("getblock", params, resp);

    cache.newAssetIssued();

    RPC::Response out;
    EXPECT_TRUE(cache.isCached("getblock", params, out));
}
