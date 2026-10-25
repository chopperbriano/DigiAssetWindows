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

// ─────────────────────────────────────────────────────────────────────────────
// appendToSideFile - every line kept, rotated to <name>.1 once full
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>

static string readAll(const string& path) {
    ifstream in(path);
    return string(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
}

TEST(Log, appendToSideFile_keepsEveryLine) {
    const string path = "_log_side_test.log";
    remove(path.c_str());
    remove((path + ".1").c_str());
    Log::appendToSideFile(path, "first", 0);
    Log::appendToSideFile(path, "second", 0);
    string text = readAll(path);
    EXPECT_NE(text.find("\tfirst\n"), string::npos);
    EXPECT_NE(text.find("\tsecond\n"), string::npos);
    remove(path.c_str());
}

TEST(Log, appendToSideFile_rotatesWhenFull) {
    const string path = "_log_side_rotate.log";
    remove(path.c_str());
    remove((path + ".1").c_str());
    //each line is ~30 bytes with its timestamp, so the 100 byte cap is passed on the 4th
    for (int i = 0; i < 4; i++) Log::appendToSideFile(path, "line" + to_string(i), 100);
    string rotated = readAll(path + ".1");
    EXPECT_NE(rotated.find("line3"), string::npos);   //the line that filled it went with it
    Log::appendToSideFile(path, "after", 100);
    string fresh = readAll(path);
    EXPECT_NE(fresh.find("after"), string::npos);
    EXPECT_EQ(fresh.find("line0"), string::npos);     //the new file starts empty
    remove(path.c_str());
    remove((path + ".1").c_str());
}
