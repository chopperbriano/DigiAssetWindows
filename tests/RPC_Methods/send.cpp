//
// Tests for RPC::Methods::send — parameter validation only.
// Actual sends require a live DigiByte node.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

TEST(RPC_send, noParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        RPC::Methods::send(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_send, tooManyParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(Json::arrayValue); // outputs
        params.append(0);                // conf_target
        params.append("economical");     // estimate_mode
        params.append(Json::objectValue);// options
        params.append("extra");          // 5th param
        params.append("sixthparam");     // too many
        RPC::Methods::send(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_send, firstParamNotArray_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("not_an_array");
        RPC::Methods::send(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}
