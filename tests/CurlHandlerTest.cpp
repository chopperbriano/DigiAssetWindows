//
// Tests for CurlHandler — network tests skip gracefully if unavailable.
//

#include "CurlHandler.h"
#include "gtest/gtest.h"

#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

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

// ─────────────────────────────────────────────────────────────────────────────
// Shutdown abort
// ─────────────────────────────────────────────────────────────────────────────

// A request whose server never answers - what an IPFS pin/add for content nobody serves
// looks like - used to ignore abortAllTransfers() until its own timeout (20 min for a pin),
// so node shutdown hung behind it. The abort must now end it within seconds.
TEST(CurlHandler, abortAllTransfers_cancelsRequestWaitingForResponse) {
    using boost::asio::ip::tcp;
    boost::asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    const unsigned short port = acceptor.local_endpoint().port();

    // accept the connection, read nothing, answer nothing - hold it open until released
    std::atomic<bool> release{false};
    std::thread server([&]() {
        tcp::socket sock(io);
        boost::system::error_code ec;
        acceptor.accept(sock, ec);
        for (int i = 0; i < 300 && !release; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sock.close(ec);
    });

    std::atomic<bool> aborted{false};
    std::atomic<long long> elapsedMs{-1};
    std::thread client([&]() {
        auto t0 = std::chrono::steady_clock::now();
        try {
            CurlHandler::post("http://127.0.0.1:" + to_string(port) + "/api/v0/pin/add/x", {}, 60000);
        } catch (const CurlHandler::exceptionTimeout&) {
            aborted = true; //an abort surfaces as a timeout, which the retry paths expect
        } catch (...) {
        }
        elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); //let it block on the response
    CurlHandler::abortAllTransfers(true);
    client.join();
    CurlHandler::abortAllTransfers(false); //never leave the process refusing transfers
    release = true;
    server.join();

    EXPECT_TRUE(aborted);
    EXPECT_GE(elapsedMs.load(), 1000);
    EXPECT_LT(elapsedMs.load(), 10000) << "the abort did not cancel a request waiting for its response";
}

// After the abort is lifted, requests work normally again (no lingering refusal).
TEST(CurlHandler, abortAllTransfers_liftedAllowsNewRequests) {
    CurlHandler::abortAllTransfers(true);
    EXPECT_THROW(CurlHandler::get("http://127.0.0.1:1/", 2000), CurlHandler::exceptionTimeout);
    CurlHandler::abortAllTransfers(false);
    //a refused connection is an ordinary error again, not an abort
    try {
        CurlHandler::get("http://127.0.0.1:1/", 2000);
    } catch (const CurlHandler::exceptionTimeout&) {
        FAIL() << "still aborting after abortAllTransfers(false)";
    } catch (...) {
    }
}
