//
// Tests for RPC::Methods::resyncmetadata — uses RPCMethodsTest fixture.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST_F(RPCMethodsTest, resyncmetadata_returnsTrue) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::resyncmetadata(params);
    Json::Value json = response.toJSON(1);
    // resyncmetadata iterates pools and repins — always returns true
    EXPECT_TRUE(json["result"].isBool());
    EXPECT_TRUE(json["result"].asBool());
}
