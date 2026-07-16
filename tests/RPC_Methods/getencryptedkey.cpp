//
// Tests for RPC::Methods::getencryptedkey.
//

#include "AppMain.h"
#include "RPC/MethodList.h"
#include "gtest/gtest.h"
#include "../tests/RPCMethods.h"

#include <jsoncpp/json/value.h>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Parameter validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(RPC_getencryptedkey, noParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        RPC::Methods::getencryptedkey(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_getencryptedkey, twoParams_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append("addr1");
        params.append("addr2");
        RPC::Methods::getencryptedkey(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(RPC_getencryptedkey, nonStringAndNonArrayParam_throws) {
    bool threw = false;
    try {
        Json::Value params = Json::arrayValue;
        params.append(42); // not a string or array
        RPC::Methods::getencryptedkey(params);
    } catch (const DigiByteException&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ─────────────────────────────────────────────────────────────────────────────
// With fixture — address with no key returns empty array
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(RPCMethodsTest, getencryptedkey_unknownAddress_returnsEmptyArray) {
    Json::Value params = Json::arrayValue;
    params.append("dgb1qnonexistentaddressxxxxxxxxxxx");
    auto response = RPC::Methods::getencryptedkey(params);
    Json::Value json = response.toJSON(1);
    // Should return array (empty for unknown addresses)
    EXPECT_TRUE(json["result"].isArray());
}

TEST_F(RPCMethodsTest, getencryptedkey_arrayInput_returnsArray) {
    Json::Value addrs = Json::arrayValue;
    addrs.append("dgb1qaddr1");
    addrs.append("dgb1qaddr2");
    Json::Value params = Json::arrayValue;
    params.append(addrs);
    auto response = RPC::Methods::getencryptedkey(params);
    Json::Value json = response.toJSON(1);
    EXPECT_TRUE(json["result"].isArray());
}
