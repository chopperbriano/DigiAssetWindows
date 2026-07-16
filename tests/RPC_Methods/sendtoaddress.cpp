//
// Tests for RPC::Methods::sendtoaddress — parameter validation only.
// Actual sends require a live DigiByte node.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST(RPC_sendtoaddress, tooFewParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("dgb1qaddress"); // only 1 param, need at least 2
        RPC::Methods::sendtoaddress(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_sendtoaddress, tooManyParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        for (int i = 0; i < 10; ++i) params.append(i); // 10 params, max is 9
        RPC::Methods::sendtoaddress(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_sendtoaddress, firstParamNotString_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(12345);   // must be a string (address)
        params.append(1.0);     // amount
        RPC::Methods::sendtoaddress(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}
