//
// Tests for RPC::Methods::asyncget — file-based, no AppMain required.
//

#include "DigiByteCore_Exception.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"

#include <jsoncpp/json/value.h>
#include <fstream>
#include <string>

using namespace std;

// asyncget reads files from a "cache/" directory relative to cwd.
// Tests verify parameter validation and the "file not found → false" path.

TEST(RPC_asyncget, invalidParams_noArgs_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        RPC::Methods::asyncget(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_asyncget, invalidParams_nonStringMethod_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(42); // method name must be a string
        RPC::Methods::asyncget(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_asyncget, missingCacheFile_returnsFalse) {
    Json::Value params = Json::arrayValue;
    params.append("nonexistent_method_that_was_never_started");
    auto response = RPC::Methods::asyncget(params);
    Json::Value json = response.toJSON(1);
    // If no cache file exists, result should be false
    EXPECT_FALSE(json["result"].asBool());
}
