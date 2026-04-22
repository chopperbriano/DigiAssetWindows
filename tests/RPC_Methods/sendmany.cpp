//
// Tests for RPC::Methods::sendmany — parameter validation only.
// Actual sends require a live DigiByte node.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST(RPC_sendmany, tooFewParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("dummy_account"); // only 1 param, need at least 2
        RPC::Methods::sendmany(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_sendmany, tooManyParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        for (int i = 0; i < 10; ++i) params.append(i); // 10 params, max is 9
        RPC::Methods::sendmany(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_sendmany, secondParamNotObject_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("account");
        params.append("not_an_object"); // must be object
        RPC::Methods::sendmany(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}
