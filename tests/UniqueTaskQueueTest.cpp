//
// Tests for UniqueTaskQueue — thread-safe, duplicate-preventing queue.
//

#include "UniqueTaskQueue.h"
#include "gtest/gtest.h"

#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// enqueue()
// ─────────────────────────────────────────────────────────────────────────────

TEST(UniqueTaskQueue, enqueue_newTask_returnsTrue) {
    UniqueTaskQueue q;
    EXPECT_TRUE(q.enqueue("task1"));
}

TEST(UniqueTaskQueue, enqueue_duplicateTask_returnsFalse) {
    UniqueTaskQueue q;
    q.enqueue("task1");
    EXPECT_FALSE(q.enqueue("task1"));
}

TEST(UniqueTaskQueue, enqueue_differentTasks_allReturnTrue) {
    UniqueTaskQueue q;
    EXPECT_TRUE(q.enqueue("a"));
    EXPECT_TRUE(q.enqueue("b"));
    EXPECT_TRUE(q.enqueue("c"));
}

// ─────────────────────────────────────────────────────────────────────────────
// isEmpty() and length()
// ─────────────────────────────────────────────────────────────────────────────

TEST(UniqueTaskQueue, isEmpty_startsEmpty) {
    UniqueTaskQueue q;
    EXPECT_TRUE(q.isEmpty());
    EXPECT_EQ(q.length(), 0u);
}

TEST(UniqueTaskQueue, length_incrementsOnEnqueue) {
    UniqueTaskQueue q;
    q.enqueue("a");
    EXPECT_EQ(q.length(), 1u);
    q.enqueue("b");
    EXPECT_EQ(q.length(), 2u);
}

TEST(UniqueTaskQueue, length_unchangedOnDuplicate) {
    UniqueTaskQueue q;
    q.enqueue("a");
    q.enqueue("a"); // duplicate
    EXPECT_EQ(q.length(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// dequeue()
// ─────────────────────────────────────────────────────────────────────────────

TEST(UniqueTaskQueue, dequeue_returnsFIFOOrder) {
    UniqueTaskQueue q;
    q.enqueue("first");
    q.enqueue("second");
    q.enqueue("third");

    EXPECT_EQ(q.dequeue(), "first");
    EXPECT_EQ(q.dequeue(), "second");
    EXPECT_EQ(q.dequeue(), "third");
}

TEST(UniqueTaskQueue, dequeue_decreasesLength) {
    UniqueTaskQueue q;
    q.enqueue("x");
    q.enqueue("y");
    q.dequeue();
    EXPECT_EQ(q.length(), 1u);
    q.dequeue();
    EXPECT_EQ(q.length(), 0u);
    EXPECT_TRUE(q.isEmpty());
}

TEST(UniqueTaskQueue, dequeue_allowsReenqueueAfterDequeue) {
    UniqueTaskQueue q;
    q.enqueue("task");
    q.dequeue();
    // After dequeue the task is no longer in the set, so re-enqueue should succeed
    EXPECT_TRUE(q.enqueue("task"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent enqueue/dequeue
// ─────────────────────────────────────────────────────────────────────────────

TEST(UniqueTaskQueue, concurrent_producerConsumer) {
    UniqueTaskQueue q;
    const int N = 50;

    // Producer: enqueue N unique tasks
    auto producer = async(launch::async, [&]() {
        for (int i = 0; i < N; ++i) {
            q.enqueue("task_" + to_string(i));
        }
    });

    // Consumer: dequeue N tasks
    auto consumer = async(launch::async, [&]() {
        vector<string> results;
        for (int i = 0; i < N; ++i) {
            results.push_back(q.dequeue());
        }
        return results;
    });

    producer.get();
    auto results = consumer.get();

    EXPECT_EQ(results.size(), static_cast<size_t>(N));
    EXPECT_TRUE(q.isEmpty());
}

TEST(UniqueTaskQueue, dequeue_blocksUntilItemAvailable) {
    UniqueTaskQueue q;

    auto future = async(launch::async, [&]() {
        return q.dequeue(); // blocks until item enqueued
    });

    // Give the async thread time to block
    this_thread::sleep_for(chrono::milliseconds(50));

    q.enqueue("delayed");

    auto result = future.get();
    EXPECT_EQ(result, "delayed");
}
