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
#include <future>

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
    try {
        startupFunction();
    } catch (const std::exception& e) {
        Log::GetInstance()->addMessage(std::string("Threaded startupFunction exception: ") + e.what(), Log::CRITICAL);
        _running = false;
        return;
    } catch (...) {
        Log::GetInstance()->addMessage("Threaded startupFunction unknown exception", Log::CRITICAL);
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
 * Override this function if there is code that should be run when the thread is started
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
