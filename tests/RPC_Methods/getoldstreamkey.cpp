//
// Tests for RPC::Methods::getoldstreamkey — parameter validation only.
// Live key lookup requires a running OldStream data source.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST(RPC_getoldstreamkey, noParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        RPC::Methods::getoldstreamkey(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_getoldstreamkey, twoParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("key1");
        params.append("key2"); // only 1 param allowed
        RPC::Methods::getoldstreamkey(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_getoldstreamkey, nonStringNonInt_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(3.14); // must be string or int
        RPC::Methods::getoldstreamkey(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}
