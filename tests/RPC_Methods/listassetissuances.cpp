//
// Tests for RPC::Methods::listassetissuances.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Parameter validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_listassetissuances, noParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        RPC::Methods::listassetissuances(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_listassetissuances, nonStringParam_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(12345); // must be a string (asset ID)
        RPC::Methods::listassetissuances(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_listassetissuances, twoParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("assetId");
        params.append("extra");
        RPC::Methods::listassetissuances(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ─────────────────────────────────────────────────────────────────────────────
// With fixture — nonexistent asset returns empty array
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(RPCMethodsTest, listassetissuances_unknownAsset_returnsEmptyArray) {
    Json::Value params = Json::arrayValue;
    params.append("La111111111111111111111111111111111111");
    auto response = RPC::Methods::listassetissuances(params);
    Json::Value json = response.toJSON(1);
    EXPECT_TRUE(json["result"].isArray());
    EXPECT_EQ(json["result"].size(), 0u);
}
