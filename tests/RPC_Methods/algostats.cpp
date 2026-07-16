//
// Tests for RPC::Methods::algostats.
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

TEST(RPC_algostats, tooManyParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(0);
        params.append(1000);
        params.append(86400);
        params.append("extra");
        RPC::Methods::algostats(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_algostats, nonIntParam_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("bad");
        RPC::Methods::algostats(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ─────────────────────────────────────────────────────────────────────────────
// With fixture
// ─────────────────────────────────────────────────────────────────────────────
/*  todo very slow.  need to see if function can be improved.
TEST_F(RPCMethodsTest, algostats_noParams_returnsArray) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::algostats(params);
    Json::Value json = response.toJSON(1);
    EXPECT_TRUE(json["result"].isArray());
}
TEST_F(RPCMethodsTest, algostats_withTimeRange_returnsArray) {
    Json::Value params = Json::arrayValue;
    params.append(0);
    params.append(9999999);
    auto response = RPC::Methods::algostats(params);
    Json::Value json = response.toJSON(1);
    EXPECT_TRUE(json["result"].isArray());
}

*/