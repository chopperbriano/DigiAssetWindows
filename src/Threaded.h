//
// Created by mctrivia on 18/08/23.
//
// Threaded.h - Reusable background-worker base class.
//
// Provides a simple "start a thread that repeatedly calls mainFunction()
// until told to stop" pattern used throughout the node and pool. Subclasses
// override startupFunction()/mainFunction()/shutdownFunction() to supply the
// actual work. Optionally the main loop can fan each iteration out onto up to
// _parallels concurrent sub-tasks (see setMaxParallels). Many long-lived
// services (chain analyzer, IPFS pinner, pool workers, etc.) derive from this.

#ifndef DIGIASSET_CORE_THREADED_H
#define DIGIASSET_CORE_THREADED_H



#include <atomic>
#include <future>
#include <thread>
#include <vector>

// Base class that owns one worker std::thread and drives the
// startup -> (repeat mainFunction) -> shutdown lifecycle. Not thread-safe to
// start()/stop() concurrently; intended to be driven from a single owner.
class Threaded {
    std::thread _thread;
    // atomic (not volatile - volatile is not a threading primitive) so the flags
    // are safely visible across the worker thread and the owner thread.
    std::atomic<bool> _running{false};
    std::atomic<bool> _stopRequest{false};
    void _threadFunction();                 // thread entry point: runs the startup/main-loop/shutdown lifecycle
    size_t _parallels = 1;//if task is asynchronous allows running sub threads within thread.

protected:
    virtual void startupFunction();         // override: one-time setup run once when the thread starts
    virtual void mainFunction();            // override: the repeatedly-executed work (called in a loop until stop)
    virtual void shutdownFunction();        // override: one-time cleanup run once when the thread stops
    void setMaxParallels(size_t max = 1);   // set how many mainFunction() sub-tasks may run concurrently per loop

public:
    bool stopRequested();                   // true once stop() has requested shutdown; poll from mainFunction() to exit early
    void start();                           // spawn the worker thread (no-op if already running)
    virtual void stop();                    // request shutdown and block until the worker thread has fully exited
    // IMPORTANT: ~Threaded() calls the virtual stop(), but during base destruction
    // the vtable is already the base's, so a subclass override does NOT run. Any
    // subclass whose stop() joins its OWN extra threads MUST also call stop() in
    // its own destructor (IPFS and ChainAnalyzer already do). Otherwise its worker
    // runs against half-destroyed members -> UAF.
    virtual ~Threaded();
};



#endif//DIGIASSET_CORE_THREADED_H
