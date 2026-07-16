//
// Tests for RPC::Methods::asyncstart — validates parameter handling.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"

#include <jsoncpp/json/value.h>
#include <string>

using namespace std;

TEST(RPC_asyncstart, invalidParams_noArgs_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        RPC::Methods::asyncstart(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_asyncstart, invalidParams_nonStringMethod_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(999); // method name must be a string
        RPC::Methods::asyncstart(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_asyncstart, validMethodName_returnsQueueLength) {
    Json::Value params = Json::arrayValue;
    params.append("version"); // valid method name (string)
    auto response = RPC::Methods::asyncstart(params);
    Json::Value json = response.toJSON(1);
    // Result should be the queue length (an integer >= 0)
    EXPECT_TRUE(json["result"].isIntegral() || json["result"].isNumeric());
}
