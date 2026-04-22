//
// Tests for RPC::Methods::syncstate — requires RPCMethodsTest fixture.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST_F(RPCMethodsTest, syncstate_returnsCountAndSync) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::syncstate(params);
    Json::Value json = response.toJSON(1);

    EXPECT_FALSE(json["result"].isNull());
    EXPECT_TRUE(json["result"].isMember("count")) << "result must have 'count' field";
    EXPECT_TRUE(json["result"].isMember("sync"))  << "result must have 'sync' field";
    EXPECT_TRUE(json["result"]["count"].isIntegral());
    EXPECT_TRUE(json["result"]["sync"].isIntegral());
}

TEST_F(RPCMethodsTest, syncstate_syncMatchesFakeAnalyzer) {
    // Fixture loads analyzer with loadFake(17579454, -1)
    // -1 means STOPPED per ChainAnalyzer constants
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::syncstate(params);
    Json::Value json = response.toJSON(1);
    EXPECT_EQ(json["result"]["sync"].asInt(), -1);
}
