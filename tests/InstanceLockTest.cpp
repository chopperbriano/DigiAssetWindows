//
// Tests for InstanceLock — single-instance enforcement via lock file.
//
// NOTE: InstanceLock uses a static file descriptor shared across all instances.
// Tests are written to account for this design, using sequential acquire/release.
//

#include "InstanceLock.h"
#include "gtest/gtest.h"

#include <string>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Basic acquire and release
// ─────────────────────────────────────────────────────────────────────────────

TEST(InstanceLock, acquire_succeeds_firstTime) {
    InstanceLock lock("test_ilock_basic");
    EXPECT_TRUE(lock.acquire());
    lock.release();
}

TEST(InstanceLock, acquire_doesNotCrash) {
    InstanceLock lock("test_ilock_nodecrash");
    bool result = lock.acquire();
    // On any supported platform, acquire should return true or false without crashing
    (void)result;
    lock.release();
}

// ─────────────────────────────────────────────────────────────────────────────
// Same lock name cannot be acquired twice from the same process
// ─────────────────────────────────────────────────────────────────────────────

TEST(InstanceLock, acquire_sameName_secondAcquireFails) {
    InstanceLock lock1("test_ilock_double");
    ASSERT_TRUE(lock1.acquire()) << "First acquire should succeed";

    InstanceLock lock2("test_ilock_double");
    bool second = lock2.acquire();
    EXPECT_FALSE(second) << "Second acquire for the same lock name should fail (process already running)";

    lock1.release();
}

// ─────────────────────────────────────────────────────────────────────────────
// Different lock names can be acquired independently
// ─────────────────────────────────────────────────────────────────────────────

// NOTE: Because _lockFileDescriptor is static, only the most recently acquired
// descriptor is tracked. This test acquires, checks, then releases immediately
// to avoid the descriptor being clobbered.
TEST(InstanceLock, acquire_differentNames_firstSucceeds) {
    InstanceLock lockA("test_ilock_nameA");
    EXPECT_TRUE(lockA.acquire());
    lockA.release();

    InstanceLock lockB("test_ilock_nameB");
    EXPECT_TRUE(lockB.acquire());
    lockB.release();
}

// ─────────────────────────────────────────────────────────────────────────────
// Re-acquire after release
// ─────────────────────────────────────────────────────────────────────────────

TEST(InstanceLock, reacquire_afterRelease_succeeds) {
    InstanceLock lock("test_ilock_reacquire");

    ASSERT_TRUE(lock.acquire());
    lock.release();

    // After release, the lock file is removed — a fresh acquire should succeed
    EXPECT_TRUE(lock.acquire());
    lock.release();
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor releases automatically
// ─────────────────────────────────────────────────────────────────────────────

TEST(InstanceLock, destructor_releasesLock) {
    {
        InstanceLock lock("test_ilock_dtor");
        ASSERT_TRUE(lock.acquire());
        // destructor called at end of scope
    }

    // After the lock object is destroyed, a new lock with the same name should succeed
    InstanceLock lock2("test_ilock_dtor");
    EXPECT_TRUE(lock2.acquire());
    lock2.release();
}
