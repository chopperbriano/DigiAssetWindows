// Regression tests for Threaded's startup lifecycle.
//
// A startupFunction() that threw used to log one CRITICAL, set _running = false and
// return - permanently killing the worker with no way to recover. In production that
// meant a node whose ChainAnalyzer lost a race with a still-warming-up DigiByte Core
// sat thousands of blocks behind for hours, while the rest of the process kept running
// and reporting itself healthy. Startup failures are transient far more often than they
// are fatal, so they must be retried.
//
// These tests pin that behaviour: startup is retried until it succeeds, the worker stays
// alive while retrying, and a stop() during the retry loop still unwinds promptly.

#include "Threaded.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace {

    /**
     * Test worker whose startupFunction() throws the first `failures` times it is called
     * and succeeds afterwards. Counts startup attempts and mainFunction() entries so a
     * test can observe whether the lifecycle actually got past startup.
     */
    class FlakyStartupWorker : public Threaded {
    public:
        explicit FlakyStartupWorker(unsigned failures)
            : _failuresRemaining(failures) {
            //keep the retry backoff sub-millisecond so the tests do not sleep for real
            _startupRetryDelayMs = 1;
            _startupRetryMaxDelayMs = 2;
        }

        ~FlakyStartupWorker() override { stop(); }

        std::atomic<unsigned> startupAttempts{0};
        std::atomic<unsigned> mainRuns{0};

    protected:
        void startupFunction() override {
            startupAttempts++;
            if (_failuresRemaining > 0) {
                _failuresRemaining--;
                throw std::runtime_error("dependency not ready yet");
            }
        }

        void mainFunction() override {
            mainRuns++;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

    private:
        std::atomic<unsigned> _failuresRemaining;
    };

    /**
     * Spin until `predicate` holds or `timeout` elapses. Returns whether it held, so a
     * failing test reports the condition rather than hanging the suite.
     */
    template<typename fn_t>
    bool waitFor(fn_t predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return predicate();
    }

}// namespace

// A startup that throws once must be retried, not treated as fatal.
TEST(ThreadedStartup, RetriesAfterATransientFailure) {
    FlakyStartupWorker worker(1);
    worker.start();

    EXPECT_TRUE(waitFor([&] { return worker.mainRuns.load() > 0; }))
            << "worker never reached mainFunction - a single startup failure killed it";
    EXPECT_GE(worker.startupAttempts.load(), 2u) << "startupFunction was not retried";
    EXPECT_TRUE(worker.isRunning());

    worker.stop();
}

// The real-world case: the dependency is down for several attempts and then comes up.
// The worker has to still be there to notice.
TEST(ThreadedStartup, RecoversOnceTheDependencyComesUp) {
    FlakyStartupWorker worker(5);
    worker.start();

    EXPECT_TRUE(waitFor([&] { return worker.mainRuns.load() > 0; }))
            << "worker gave up instead of waiting for the dependency";
    EXPECT_GE(worker.startupAttempts.load(), 6u);

    worker.stop();
}

// While startup keeps failing the worker must report itself running, so the state is
// "still trying" rather than the silent death that used to look identical to healthy.
TEST(ThreadedStartup, StaysAliveWhileStartupKeepsFailing) {
    FlakyStartupWorker worker(100000);// effectively never succeeds
    worker.start();

    EXPECT_TRUE(waitFor([&] { return worker.startupAttempts.load() >= 3; }))
            << "startup was not retried repeatedly";
    EXPECT_TRUE(worker.isRunning()) << "worker died instead of continuing to retry";
    EXPECT_EQ(worker.mainRuns.load(), 0u) << "mainFunction ran despite startup never succeeding";

    worker.stop();
    EXPECT_FALSE(worker.isRunning());
}

// Retrying forever must not make shutdown hang: stop() has to break the retry loop.
TEST(ThreadedStartup, StopUnwindsWhileRetryingStartup) {
    FlakyStartupWorker worker(100000);
    worker.start();
    ASSERT_TRUE(waitFor([&] { return worker.startupAttempts.load() >= 2; }));

    const auto begin = std::chrono::steady_clock::now();
    worker.stop();
    const auto took = std::chrono::steady_clock::now() - begin;

    EXPECT_FALSE(worker.isRunning());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(took).count(), 5)
            << "stop() blocked waiting out the startup backoff";
}

// A worker that starts cleanly must be unaffected by the retry path.
TEST(ThreadedStartup, CleanStartupRunsExactlyOnce) {
    FlakyStartupWorker worker(0);
    worker.start();

    EXPECT_TRUE(waitFor([&] { return worker.mainRuns.load() > 0; }));
    worker.stop();

    EXPECT_EQ(worker.startupAttempts.load(), 1u) << "startupFunction ran more than once on a clean start";
}
