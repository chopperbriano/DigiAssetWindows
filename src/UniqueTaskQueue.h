//
// Created by mctrivia on 08/03/24.
//
// UniqueTaskQueue.h - Thread-safe FIFO queue of string task identifiers that
// silently de-duplicates.
//
// A producer/consumer work queue where each task string is present at most
// once: enqueuing a task already waiting in the queue is a no-op. Used to
// schedule work (e.g. content/asset identifiers to fetch or pin) from many
// producer threads while consumer threads block on dequeue() for the next
// unique item. Backed by a std::queue for ordering plus an unordered_set for
// O(1) membership tests, guarded by a mutex and a condition variable.

#ifndef DIGIASSET_CORE_UNIQUETASKQUEUE_H
#define DIGIASSET_CORE_UNIQUETASKQUEUE_H



#include <queue>
#include <unordered_set>
#include <mutex>
#include <string>
#include <condition_variable>
// Thread-safe, de-duplicating FIFO task queue. All public methods lock _mutex.
class UniqueTaskQueue {
private:
    std::mutex _mutex;
    std::condition_variable _cond;
    std::queue<std::string> _queue;
    std::unordered_set<std::string> _set;

public:
    // Adds a task to the queue if it's not already present
    bool enqueue(const std::string& task);

    // Retrieves and removes the next task from the queue
    // Blocks if the queue is empty until a new task is added
    std::string dequeue();

    // Checks if the queue is empty (primarily for testing or conditional processing)
    bool isEmpty();

    // Returns the current number of tasks waiting in the queue
    unsigned int length();
};



#endif //DIGIASSET_CORE_UNIQUETASKQUEUE_H
