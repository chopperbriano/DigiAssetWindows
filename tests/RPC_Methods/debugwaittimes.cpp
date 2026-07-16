//
// Tests for RPC::Methods::debugwaittimes — requires RPCMethodsTest fixture.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST_F(RPCMethodsTest, debugwaittimes_returnsArray) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::debugwaittimes(params);
    Json::Value json = response.toJSON(1);

    EXPECT_FALSE(json["result"].isNull());
    EXPECT_TRUE(json["result"].isArray()) << "debugwaittimes result must be an array of strings";
}

TEST_F(RPCMethodsTest, debugwaittimes_arrayContainsStrings) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::debugwaittimes(params);
    Json::Value json = response.toJSON(1);
    Json::Value arr = json["result"];

    for (const auto& entry : arr) {
        EXPECT_TRUE(entry.isString()) << "Each debug wait time entry should be a string";
    }
}
