//
// Created by mctrivia on 08/03/24.
//
// UniqueTaskQueue.cpp - Implementation of the thread-safe de-duplicating task
// queue declared in UniqueTaskQueue.h. Membership is tracked in _set so a task
// already queued is ignored on enqueue; consumers block in dequeue() on the
// condition variable until an item is available. See the header for the class
// role in the node/pool.

#include "UniqueTaskQueue.h"
#include <queue>
#include <unordered_set>
#include <mutex>
#include <string>
#include <condition_variable>

using namespace std;

// Adds a task to the queue if it's not already present.
// Returns true and wakes one waiting consumer if the task was newly added;
// returns false (no change) if an identical task is already queued.
bool UniqueTaskQueue::enqueue(const string& task) {
    unique_lock<mutex> lock(_mutex);
    if (_set.find(task) == _set.end()) {
        _queue.push(task);
        _set.insert(task);
        lock.unlock();
        _cond.notify_one(); // Notify one waiting thread
        return true;
    }
    return false; // Task was already in the set
}

// Retrieves and removes the next task from the queue (FIFO order).
// Blocks if the queue is empty until a new task is added. Also removes the
// task from the de-dup set so the same identifier may be enqueued again later.
string UniqueTaskQueue::dequeue() {
    unique_lock<mutex> lock(_mutex);
    _cond.wait(lock, [this] { return !_queue.empty(); }); // Wait until the queue is not empty

    string task = _queue.front();
    _queue.pop();
    _set.erase(task);
    return task;
}

// Checks if the queue is empty (primarily for testing or conditional processing)
bool UniqueTaskQueue::isEmpty() {
    lock_guard<mutex> lock(_mutex);
    return _queue.empty();
}

// Returns the current number of tasks waiting in the queue.
unsigned int UniqueTaskQueue::length() {
    lock_guard<mutex> lock(_mutex);
    return _queue.size();
}
