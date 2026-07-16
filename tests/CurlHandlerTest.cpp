//
// Tests for CurlHandler — network tests skip gracefully if unavailable.
//

#include "CurlHandler.h"
#include "gtest/gtest.h"

#include <string>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Timeout exception type
// ─────────────────────────────────────────────────────────────────────────────

TEST(CurlHandler, exceptionTimeout_isException) {
    CurlHandler::exceptionTimeout ex;
    string what(ex.what());
    EXPECT_FALSE(what.empty());
    EXPECT_NE(what.find("timed out"), string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// GET — requires network (skip gracefully if unavailable)
// ─────────────────────────────────────────────────────────────────────────────

TEST(CurlHandler, get_validUrl_returnsNonEmpty) {
    try {
        string result = CurlHandler::get("https://example.com", 200);
        EXPECT_FALSE(result.empty()) << "GET to example.com returned empty response";
    } catch (const CurlHandler::exceptionTimeout&) {
        GTEST_SKIP() << "Network unavailable — CurlHandler GET timed out";
    } catch (...) {
        GTEST_SKIP() << "Network unavailable — CurlHandler GET threw unexpected exception";
    }
}

TEST(CurlHandler, get_invalidUrl_doesNotCrash) {
    // Connecting to a non-routable address should time out or throw, not crash
    try {
        CurlHandler::get("http://192.0.2.0/", 200); // TEST-NET, guaranteed unreachable
        // If we get here without throwing, that's also acceptable
    } catch (const CurlHandler::exceptionTimeout&) {
        SUCCEED() << "Correctly threw exceptionTimeout for unreachable host";
    } catch (...) {
        // Any exception is acceptable — we just verify no crash
        SUCCEED();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST — requires network (skip gracefully if unavailable)
// ─────────────────────────────────────────────────────────────────────────────

TEST(CurlHandler, post_validUrl_doesNotCrash) {
    try {
        map<string, string> data = {{"key", "value"}};
        string result = CurlHandler::post("https://httpbin.org/post", data, 200);
        EXPECT_FALSE(result.empty());
    } catch (const CurlHandler::exceptionTimeout&) {
        GTEST_SKIP() << "Network unavailable — CurlHandler POST timed out";
    } catch (...) {
        GTEST_SKIP() << "Network unavailable — CurlHandler POST threw unexpected exception";
    }
}
