//
// Tests for RPC::Methods::shutdown — uses RPCMethodsTest fixture.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST_F(RPCMethodsTest, shutdown_returnsTrue) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::shutdown(params);
    Json::Value json = response.toJSON(1);
    // shutdown always returns true
    EXPECT_TRUE(json["result"].isBool());
    EXPECT_TRUE(json["result"].asBool());
}
