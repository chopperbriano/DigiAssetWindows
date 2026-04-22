//
// Tests for RPC::Methods::asyncclear — file-based, no AppMain required.
//

#include "DigiByteCore_Exception.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"

#include <jsoncpp/json/value.h>
#include <string>

using namespace std;

TEST(RPC_asyncclear, invalidParams_noArgs_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        RPC::Methods::asyncclear(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_asyncclear, invalidParams_nonStringMethod_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(123);
        RPC::Methods::asyncclear(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_asyncclear, missingCacheFile_returnsFalse) {
    Json::Value params = Json::arrayValue;
    params.append("definitely_nonexistent_method_xyz");
    auto response = RPC::Methods::asyncclear(params);
    Json::Value json = response.toJSON(1);
    EXPECT_FALSE(json["result"].asBool());
}
