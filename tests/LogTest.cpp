//
// Tests for Log singleton — no external dependencies required.
//
// NOTE: Log uses a process-lifetime singleton with a single ofstream. By the
// time these tests run, DigiAssetTransactionTest has already called
// Log::GetInstance("debug.log"), permanently binding the singleton's _logFile.
// Attempting to redirect it via setLogFile() silently fails (ofstream::open on
// an already-open stream is a no-op). File-output tests are therefore not
// feasible in this test binary. The tests below cover the observable API.
//

#include "Log.h"
#include "gtest/gtest.h"

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Singleton behaviour
// ─────────────────────────────────────────────────────────────────────────────

TEST(Log, getInstance_returnsNonNull) {
    Log* log = Log::GetInstance();
    EXPECT_NE(log, nullptr);
}

TEST(Log, getInstance_returnsSamePointerEachCall) {
    Log* a = Log::GetInstance();
    Log* b = Log::GetInstance();
    EXPECT_EQ(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// addMessage — smoke tests (should not throw for any level)
// ─────────────────────────────────────────────────────────────────────────────

TEST(Log, addMessage_doesNotThrow_debug) {
    EXPECT_NO_THROW(Log::GetInstance()->addMessage("test", Log::DEBUG));
}

TEST(Log, addMessage_doesNotThrow_info) {
    EXPECT_NO_THROW(Log::GetInstance()->addMessage("test", Log::INFO));
}

TEST(Log, addMessage_doesNotThrow_warning) {
    EXPECT_NO_THROW(Log::GetInstance()->addMessage("test", Log::WARNING));
}

TEST(Log, addMessage_doesNotThrow_error) {
    EXPECT_NO_THROW(Log::GetInstance()->addMessage("test", Log::ERROR));
}

TEST(Log, addMessage_doesNotThrow_critical) {
    EXPECT_NO_THROW(Log::GetInstance()->addMessage("test", Log::CRITICAL));
}

// ─────────────────────────────────────────────────────────────────────────────
// Level constants
// ─────────────────────────────────────────────────────────────────────────────

TEST(Log, levelConstants_areOrdered) {
    EXPECT_LT(+Log::DEBUG,   +Log::INFO);
    EXPECT_LT(+Log::INFO,    +Log::WARNING);
    EXPECT_LT(+Log::WARNING, +Log::ERROR);
    EXPECT_LT(+Log::ERROR,   +Log::CRITICAL);
}

// ─────────────────────────────────────────────────────────────────────────────
// setMinLevel — should not throw
// ─────────────────────────────────────────────────────────────────────────────

TEST(Log, setMinLevelToScreen_doesNotThrow) {
    Log* log = Log::GetInstance();
    EXPECT_NO_THROW(log->setMinLevelToScreen(Log::DEBUG));
    EXPECT_NO_THROW(log->setMinLevelToScreen(Log::INFO));   // restore default
}

TEST(Log, setMinLevelToFile_doesNotThrow) {
    Log* log = Log::GetInstance();
    EXPECT_NO_THROW(log->setMinLevelToFile(Log::DEBUG));
    EXPECT_NO_THROW(log->setMinLevelToFile(Log::INFO));     // restore default
}

// ─────────────────────────────────────────────────────────────────────────────
// Exception class — what() returns a non-empty message
// ─────────────────────────────────────────────────────────────────────────────

TEST(Log, exceptionFailedToOpen_whatIsNonEmpty) {
    Log::exceptionFailedToOpen ex;
    string msg = ex.what();
    EXPECT_FALSE(msg.empty());
}
