//
// Tests for DigiByteException (DigiByteCore_Exception.h) — no external dependencies.
//
// Regression: the constructor used to run EVERY message through the bitcoin-style
// JSON error-blob parser, so directly thrown exceptions (e.g.
// DigiByteException(RPC_INVALID_PARAMS, "Invalid params")) came out with
// code -1 and message "Error during parsing of >>Invalid params<<".
// Explicit code/message pairs must be preserved verbatim; the parser must only
// kick in when the message actually contains an {"error":{...}} blob.
//

#include "DigiByteCore_Exception.h"
#include "RPC/Server.h"
#include "gtest/gtest.h"

TEST(DigiByteException, preservesExplicitCodeAndMessage) {
    DigiByteException e(RPC_INVALID_PARAMS, "Invalid params");
    EXPECT_EQ(e.getCode(), RPC_INVALID_PARAMS);
    EXPECT_EQ(e.getMessage(), "Invalid params");
}

TEST(DigiByteException, preservesHttpStyleCodes) {
    DigiByteException unauthorized(HTTP_UNAUTHORIZED, "Invalid HTTP request: No valid authentication provided.");
    EXPECT_EQ(unauthorized.getCode(), HTTP_UNAUTHORIZED);

    DigiByteException forbidden(RPC_FORBIDDEN_BY_SAFE_MODE, "shutdown is forbidden");
    EXPECT_EQ(forbidden.getCode(), RPC_FORBIDDEN_BY_SAFE_MODE);
    EXPECT_EQ(forbidden.getMessage(), "shutdown is forbidden");
}

TEST(DigiByteException, parsesWalletErrorBlob) {
    // format jsonrpccpp produces when DigiByte Core rejects a call
    DigiByteException e(-32603, R"(INTERNAL_ERROR: : {"error":{"code":-6,"message":"Error: Insufficient funds"}})");
    EXPECT_EQ(e.getCode(), -6);
    EXPECT_EQ(e.getMessage(), "Insufficient funds");
}

TEST(DigiByteException, parsesBareErrorBlob) {
    DigiByteException e(-32603, R"({"error":{"code":-5,"message":"Invalid address"}})");
    EXPECT_EQ(e.getCode(), -5);
    EXPECT_EQ(e.getMessage(), "Invalid address");
}

TEST(DigiByteException, emptyBlobMessageDoesNotCrash) {
    DigiByteException e(-32603, R"({"error":{"code":-7,"message":""}})");
    EXPECT_EQ(e.getCode(), -7);
    EXPECT_EQ(e.getMessage(), "");
}
