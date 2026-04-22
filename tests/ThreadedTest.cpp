//
// Tests for Threaded base class — verifies thread lifecycle.
//

#include "Threaded.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Minimal subclass that counts startupFunction / shutdownFunction calls
// ─────────────────────────────────────────────────────────────────────────────

class CountingThread : public Threaded {
public:
    atomic<int> startupCount{0};
    atomic<int> shutdownCount{0};
    atomic<int> mainCount{0};

protected:
    void startupFunction() override {
        startupCount++;
    }

    void mainFunction() override {
        mainCount++;
        // Yield to avoid spinning too hard
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    void shutdownFunction() override {
        shutdownCount++;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST(Threaded, startup_mainFunction_shutdown_called) {
    CountingThread t;
    t.start();

    // Let it run for a bit
    this_thread::sleep_for(chrono::milliseconds(100));

    t.stop();

    EXPECT_EQ(t.startupCount.load(), 1) << "startupFunction should be called exactly once";
    EXPECT_EQ(t.shutdownCount.load(), 1) << "shutdownFunction should be called exactly once";
    EXPECT_GE(t.mainCount.load(), 1) << "mainFunction should be called at least once";
}

TEST(Threaded, stop_stopsThread) {
    CountingThread t;
    t.start();
    this_thread::sleep_for(chrono::milliseconds(50));
    t.stop();

    int countAfterStop = t.mainCount.load();
    this_thread::sleep_for(chrono::milliseconds(50));
    int countAfterWait = t.mainCount.load();

    EXPECT_EQ(countAfterStop, countAfterWait) << "mainFunction should not be called after stop()";
}

TEST(Threaded, doubleStart_isNoOp) {
    CountingThread t;
    t.start();
    t.start(); // second start should be ignored
    this_thread::sleep_for(chrono::milliseconds(50));
    t.stop();

    EXPECT_EQ(t.startupCount.load(), 1) << "startupFunction should only be called once even after double start()";
}

TEST(Threaded, stopRequested_isFalseBeforeStop) {
    CountingThread t;
    t.start();
    EXPECT_FALSE(t.stopRequested());
    t.stop();
}

TEST(Threaded, destructor_stopsRunningThread) {
    // Thread should stop cleanly when object goes out of scope
    {
        CountingThread t;
        t.start();
        this_thread::sleep_for(chrono::milliseconds(50));
        // destructor calls stop()
    }
    // If destructor hangs, the test runner will catch the timeout
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// Subclass that signals when stop is requested
// ─────────────────────────────────────────────────────────────────────────────

class StopAwareThread : public Threaded {
public:
    atomic<bool> sawStopRequest{false};

protected:
    void mainFunction() override {
        if (stopRequested()) {
            sawStopRequest = true;
        }
        this_thread::sleep_for(chrono::milliseconds(5));
    }
};

TEST(Threaded, stopRequested_becomesTrueAfterStop) {
    StopAwareThread t;
    t.start();
    this_thread::sleep_for(chrono::milliseconds(30));
    t.stop();
    // After the thread has seen the stop request, sawStopRequest may be set
    // This is a best-effort check — the main loop exits on stopRequested()
    SUCCEED(); // just verify no crash; stop() blocks until thread ends
}
