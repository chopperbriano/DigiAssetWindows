//
// Created by mctrivia on 18/08/23.
//
// Threaded.cpp - Implementation of the Threaded background-worker base class.
//
// Runs the worker lifecycle on a dedicated std::thread: startupFunction() once,
// then mainFunction() repeatedly (optionally fanned out across up to _parallels
// std::async sub-tasks per iteration) until stop() sets _stopRequest, then
// shutdownFunction() once. Exceptions escaping the overridable functions are
// caught and logged as CRITICAL so a single failing iteration cannot crash the
// whole node/pool. See Threaded.h for the subclass override points.

#include "Threaded.h"
#include "Log.h"
#include <algorithm>
#include <future>
#include <string>

using namespace std;

/**
 * Worker-thread entry point. Runs startupFunction() once (aborting the thread
 * if it throws), then loops launching mainFunction() as std::async sub-tasks,
 * capping concurrency at _parallels and reaping finished futures, until
 * _stopRequest is set. Waits for all outstanding sub-tasks, calls
 * shutdownFunction(), then clears _stopRequest and _running so the thread can
 * be started again. All exceptions from the overridable functions are logged.
 */
void Threaded::_threadFunction() {
    //startup thread
    //
    // A startup failure is RETRIED, not fatal. startupFunction() overrides depend on
    // things that are often not ready at the instant the worker launches - DigiByte
    // Core RPC still warming up, the database mid-recovery, IPFS not yet listening.
    // Aborting here killed the worker for the life of the process with no way back:
    // the rest of the node kept running and reporting itself healthy while the work
    // this thread exists to do silently never happened (a node sat 3,194 blocks behind
    // for hours showing "Initializing..." and a 100% progress bar). Backing off and
    // trying again self-heals the moment the dependency comes up.
    if (!_runStartupWithRetry()) {
        //stop() was requested while we were waiting - never started, so just unwind
        try {
            shutdownFunction();
        } catch (...) {}
        _stopRequest = false;
        _running = false;
        return;
    }

    //main function
    vector<future<void>> subThreads;
    while (!_stopRequest) {
        //run sub thread task
        subThreads.push_back(async(launch::async, [this] {
            try {
                mainFunction();
            } catch (const std::exception& e) {
                Log::GetInstance()->addMessage(std::string("Threaded mainFunction exception: ") + e.what(), Log::CRITICAL);
            } catch (...) {
                Log::GetInstance()->addMessage("Threaded mainFunction unknown exception", Log::CRITICAL);
            }
        }));

        //wait for a sub thread to be finished if full
        while (subThreads.size() >= _parallels) {
            auto it = subThreads.begin();
            while (it != subThreads.end()) {
                if (it->wait_for(chrono::seconds(0)) == future_status::ready) {
                    it = subThreads.erase(it);
                }
                else {
                    ++it;
                }
            }
            this_thread::sleep_for(chrono::milliseconds(10));// Avoid busy-waiting
        }
    }

    //wait for all sub threads to be done
    for (auto& future: subThreads) {
        future.wait();
    }

    //shutdown
    shutdownFunction();
    _stopRequest = false;
    _running = false;
}

/**
 * Runs startupFunction() until it completes without throwing, backing off between
 * attempts so a dependency that is still coming up is waited out rather than
 * treated as a permanent failure.
 *
 * Logging is deliberately graduated: the first couple of failures are WARNINGs
 * (a warming-up node is normal and self-corrects), and one CRITICAL is raised once
 * the worker has clearly failed to start so an operator still gets alerted. It is
 * NOT repeated every attempt - a stuck worker would otherwise fill the log.
 *
 * @return true once startup succeeded, false if stop() was requested while waiting
 */
bool Threaded::_runStartupWithRetry() {
    using namespace std::chrono;
    constexpr unsigned ALERT_ON_ATTEMPT = 3;   //raise the one CRITICAL here

    unsigned attempt = 0;
    unsigned delayMs = std::max(1u, _startupRetryDelayMs);
    const unsigned maxDelayMs = std::max(delayMs, _startupRetryMaxDelayMs);
    while (!_stopRequest) {
        std::string error;
        try {
            startupFunction();
            if (attempt > 0) {
                Log::GetInstance()->addMessage(
                        "Threaded startupFunction succeeded on attempt " + std::to_string(attempt + 1) +
                                " - worker is running normally.",
                        Log::INFO);
            }
            return true;
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown exception";
        }

        attempt++;
        // Report the cause AND the fact that we will try again, so the log never
        // reads as a dead end the way the old bare CRITICAL did.
        const std::string message = "Threaded startupFunction exception: " + error +
                                    " (attempt " + std::to_string(attempt) + ", retrying in " +
                                    std::to_string(delayMs) + "ms)";
        Log::GetInstance()->addMessage(message, (attempt == ALERT_ON_ATTEMPT) ? Log::CRITICAL : Log::WARNING);

        //sleep in slices so stop() is honoured promptly instead of after a full backoff
        const unsigned slice = std::min(delayMs, 100u);
        for (unsigned waited = 0; (waited < delayMs) && !_stopRequest; waited += slice) {
            std::this_thread::sleep_for(milliseconds(slice));
        }
        if (delayMs < maxDelayMs) delayMs = std::min(delayMs * 2, maxDelayMs);
    }
    return false;
}

/**
 * Override this function if there is code that should be run when the thread is started
 *
 * MUST be safe to call more than once: a throwing startup is retried (see
 * _runStartupWithRetry), so an override has to be idempotent rather than assuming
 * it only ever runs on a clean slate.
 */
void Threaded::startupFunction() {
}

/**
 * Override this function with the code that should be run continuously
 * It will keep getting executed until the thread is shut down
 */
void Threaded::mainFunction() {
    // if using sub treads and want to make sure 2 don't execute a part at the same time add
    //mutex _mutex; to your private section in header file and the following to the mainFunction where
    //you wish to prevent time overlap
    //unique_lock<mutex> lock(_mutex);
    //critical code
    //lock.unlock();
}

/**
 * Override this function if there is code that should be run when the thread is shut down
 */
void Threaded::shutdownFunction() {
}

/**
 * Starts the thread
 */
void Threaded::start() {
    //don't allow loop to run twice
    if (_running) return;
    _running = true;

    //load loop in thread
    _thread = thread(&Threaded::_threadFunction, this);
}

/**
 * Ends the thread
 */
void Threaded::stop() {
    if (_running) {
        _stopRequest = true;
        while (_running) {
            chrono::milliseconds dura(100);
            this_thread::sleep_for(dura);
        }
        _thread.join();
        _stopRequest = false;
    }
}

/**
 * makes sure the thread shuts down correctly
 */
Threaded::~Threaded() {
    stop();
}

/**
 * Sets how many mainFunction() sub-tasks the loop may run concurrently. With
 * the default of 1 the loop is effectively serial (one iteration at a time).
 * @param max maximum number of in-flight sub-task futures
 */
void Threaded::setMaxParallels(size_t max) {
    _parallels = max;
}

/**
 * Allows main function to check if it should allow shutdown
 * @return
 */
bool Threaded::stopRequested() {
    return _stopRequest;
}

/**
 * True while the worker thread is alive (from start() until the lifecycle has fully
 * unwound). Lets an owner or watchdog distinguish "this service is running" from
 * "this service died and nothing noticed" without inspecting subclass state.
 * @return whether the worker thread is currently running
 */
bool Threaded::isRunning() const {
    return _running;
}
