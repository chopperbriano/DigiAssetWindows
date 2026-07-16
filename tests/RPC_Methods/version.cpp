//
// Tests for RPC::Methods::version — no AppMain required.
//

#include "RPC/MethodList.h"
#include "gtest/gtest.h"

#include <jsoncpp/json/value.h>
#include <string>

using namespace std;

TEST(RPC_version, returnsNonEmptyString) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::version(params);
    Json::Value json = response.toJSON(1);
    EXPECT_FALSE(json["result"].isNull());
    EXPECT_TRUE(json["result"].isString());
    EXPECT_FALSE(json["result"].asString().empty());
}

TEST(RPC_version, errorFieldIsNull) {
    Json::Value params = Json::arrayValue;
    auto response = RPC::Methods::version(params);
    Json::Value json = response.toJSON(1);
    EXPECT_TRUE(json["error"].isNull());
}

TEST(RPC_version, ignoredExtraParams) {
    // version ignores any params passed — should not throw
    Json::Value params = Json::arrayValue;
    params.append("ignored");
    EXPECT_NO_THROW(RPC::Methods::version(params));
}
