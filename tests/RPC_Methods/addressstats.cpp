//
// Tests for RPC::Methods::addressstats.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Parameter validation (no fixture needed — throws before hitting database)
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_addressstats, tooManyParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(0);
        params.append(1000);
        params.append(86400);
        params.append("extra");
        RPC::Methods::addressstats(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_addressstats, nonIntParam_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("not_an_int");
        RPC::Methods::addressstats(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ─────────────────────────────────────────────────────────────────────────────
// With fixture — verifies the method runs against the test database
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(RPCMethodsTest, addressstats_noParams_returnsArray) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::addressstats(params);
    Json::Value json = response.toJSON(1);
    EXPECT_TRUE(json["result"].isArray());
}

TEST_F(RPCMethodsTest, addressstats_withStartEnd_returnsArray) {
    Json::Value params = Json::arrayValue;
    params.append(0);
    params.append(9999999);
    auto response = RPC::Methods::addressstats(params);
    Json::Value json = response.toJSON(1);
    EXPECT_TRUE(json["result"].isArray());
}
