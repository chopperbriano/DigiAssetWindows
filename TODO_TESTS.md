# Test TODO — DigiAsset Core
*Written for a fresh context session. Contains known bugs and tests to add.*

---

## Background: What Was Investigated

The test `DigiAssetTransaction.existingAssetTransactions` was crashing with **SIGILL (exit 132)**
at ~99.7% completion. Root cause investigation and fixes were applied. The test now passes
all 198,557 historical transactions. See `INVESTIGATION_SIGILL.md` for the full crash analysis.

The **`fast` branch** is the working branch. All changes are uncommitted (use `git diff HEAD` to review them).

---

## Bugs Found — Status Summary

| # | File | Bug | Fixed? |
|---|------|-----|--------|
| 1 | `src/PermanentStoragePool/PermanentStoragePool.h` | Missing virtual destructor → SIGILL | ✅ Fixed |
| 2 | `src/PermanentStoragePool/pools/mctrivia.cpp` | `keepAliveTask()` 20-min uninterruptible sleep | ✅ Fixed |
| 3 | `tests/DigiAssetTransactionTest.cpp` | IPFS worker left running after `main->reset()` → 481K CRITICAL log spam | ✅ Fixed |
| 4 | `tests/DigiAssetTransactionTest.cpp` | Missing WAL/SHM cleanup between test runs | ✅ Fixed |
| 5 | `src/PermanentStoragePool/pools/mctrivia.cpp` | Missing comma in address set — silently concatenates 2 DGB addresses | ❌ **NOT FIXED** |
| 6 | `src/PermanentStoragePool/pools/mctrivia.cpp` | `updateBadList()` adds each entry twice | ❌ **NOT FIXED** |
| 7 | `src/PermanentStoragePool/pools/mctrivia.h/.cpp` | Data race: keep-alive thread writes `_badAssets`/`_badFiles` while main thread reads | ❌ **NOT FIXED** |
| 8 | `tests/RPCMethods.cpp` | `TearDownTestSuite` deletes objects while AppMain holds raw pointers (dangling) | ❌ **NOT FIXED** |

---

## Bug Details (Unfixed)

### Bug 5 — Missing Comma in PSP Address Set
**File:** `src/PermanentStoragePool/pools/mctrivia.cpp`
**Lines:** 166–180

```cpp
const std::set<std::string> addresses = {
    "dgb1q84h0g4lpy0prppc2507wf7ngne26thza0sntgr",
    "dgb1q8c6p9nht8055lr5fczcvc4v29hunluqv3n3gaf",
    "dgb1qatvzudt2jey06kx8zn3a6p0nw689s9dxkjp57g"   // <-- MISSING COMMA HERE
    "dgb1qfc9029kc8ptvqt2nuqe4sxtps2nd83kq7pugtm",
    ...
};
```

**Effect:** C++ silently concatenates adjacent string literals. The set ends up containing
one bogus 130-character string instead of two valid 42-character addresses. Transactions
paying to either `dgb1qatvzudt2jey06kx8zn3a6p0nw689s9dxkjp57g` or
`dgb1qfc9029kc8ptvqt2nuqe4sxtps2nd83kq7pugtm` will NOT be recognized as PSP payments
and will return `""` from `serializeMetaProcessor()`.

**Fix:** Add the missing comma after line 169.

---

### Bug 6 — Duplicate Entries in `updateBadList()`
**File:** `src/PermanentStoragePool/pools/mctrivia.cpp`
**Lines:** 278–296

```cpp
for (const Json::Value& value: root["assets"]) {
    string assetId = value.asString();
    if (find(_badAssets.begin(), _badAssets.end(), assetId) == _badAssets.end()) {
        reportAssetBad(assetId, true);
        _badAssets.push_back(assetId);   // pushed once if new
    }
    _badAssets.push_back(value.asString()); // BUG: always pushed, regardless of if block
}
```

Same pattern in the `root["cids"]` loop below (lines 289–296).

**Effect:** Every new bad asset/CID gets pushed **twice** into the vector. The `find()` check
on subsequent calls finds the duplicate on its first search hit, so the dedup logic works
despite duplicates — but the vectors grow at 2× the expected rate.

**Fix:** Remove the unconditional `push_back` outside the `if` block (lines 285 and 295).

---

### Bug 7 — Data Race on `_badAssets` / `_badFiles`
**File:** `src/PermanentStoragePool/pools/mctrivia.h` / `mctrivia.cpp`

**Thread 1** (keep-alive thread): `keepAliveTask()` → `_callServer(KEEP_ALIVE)` → `updateBadList()`
→ **writes** `_badAssets` and `_badFiles`

**Thread 2** (main/chain-analyzer thread): `isAssetBad()` → `updateBadList()` (if stale) / `find()`
→ **reads and conditionally writes** `_badAssets`

Neither path holds a mutex. This is a data race (UB) under `-fsanitize=thread`.

**Fix:** Add a `std::mutex _badListMutex` member and lock it in `updateBadList()` and
`isAssetBad()`.

---

### Bug 8 — RPCMethods Teardown Leaves AppMain with Dangling Pointers
**File:** `tests/RPCMethods.cpp`
**Lines:** 47–54

```cpp
void RPCMethodsTest::TearDownTestSuite() {
    delete dgb;       // AppMain::_dgb is now dangling
    delete ipfs;      // AppMain::_ipfs is now dangling (and IPFS thread stops AFTER this)
    delete psp;       // AppMain::_psp is now dangling
    delete db;        // AppMain::_db is now dangling
    delete analyzer;
    delete rpcCache;
    // appMain->reset() is NEVER called
}
```

Additionally, `dgb` is deleted **before** IPFS is stopped. If the IPFS worker thread fires
between `delete dgb` and `delete ipfs`, it could call `AppMain::getDigiByteCore()` and
receive the dangling `_dgb` pointer.

**Fix:**
1. Call `ipfs->stop()` explicitly before any other deletion
2. Call `appMain->reset()` after all deletions to zero out AppMain's pointers

---

## Tests to Write

### New File: `tests/PermanentStoragePoolTest.cpp`

This file does not yet exist. Create it with the following test cases.
You can look at `tests/DigiAssetTest.cpp` for an example of test structure.

---

**Test 1 — `mctrivia_destructorCalledViaBasePointer`**

Regression test for Bug 1 (SIGILL fix).

```
Setup: Create mctrivia on the heap. Assign to PermanentStoragePool*.
       Call start() to launch the keep-alive thread.
Action: delete via the base pointer. Measure elapsed time.
Assert: - No crash (the destructor must be virtual for this to work)
        - Elapsed time < 3 seconds (thread must join, not block 20 minutes)
```

Without the `virtual` destructor fix this test either crashes or hangs.

---

**Test 2 — `mctrivia_stopReturnsQuickly`**

Regression test for Bug 2 (interruptible sleep fix).

```
Setup: Create mctrivia object directly (not via base pointer).
       Call start(). Sleep for 100ms to let the keep-alive thread start.
Action: Record time. Call stop(). Record time again.
Assert: elapsed < 2000ms
```

Before the fix (20-minute sleep), `stop()` would block for up to 20 minutes.

---

**Test 3 — `mctrivia_allAddressesRecognized`**

Regression/detection test for Bug 5 (missing comma).

This test requires a mock DigiByteTransaction that has an output to a specific address.
Look at `DigiByteTransaction` to see how to construct one, or use a minimal fake.

The 14 expected addresses (correct list, as they should be):
```
dgb1q84h0g4lpy0prppc2507wf7ngne26thza0sntgr
dgb1q8c6p9nht8055lr5fczcvc4v29hunluqv3n3gaf
dgb1qatvzudt2jey06kx8zn3a6p0nw689s9dxkjp57g   ← currently broken (missing comma)
dgb1qfc9029kc8ptvqt2nuqe4sxtps2nd83kq7pugtm   ← currently broken (missing comma)
dgb1qhucf64cleqdme9637vukgxau8aflpk00thlq98
dgb1qj4glly6ka7py8pkdme9t0vh77s0gym0vq2esee
dgb1qjnzadu643tsfzjqjydnh06s9lgzp3m4sg3j68x
dgb1qkqggn9y85tlyxdfhg9ls3ygph4nd58j0acnlz6
dgb1qm4putt429lu9mlc6ypukky0fq3q9spm7pjwcy8
dgb1qnseslpvugsxcnvmz7m4emvmlgeryg80ujduspw
dgb1qnynkfl44ztsw3et6rq9yhxmefrcm8ufd3afm3e
dgb1qva97ew3zdwyadm5aqstqxe6xzzgxmxm7d6m3uw
dgb1qxhx0ahcmuxxmlwvnkjdq6dhmnem570g587m7hk
dgb1qylaqaen0jqs2sk7jlc74yarw5lg4nzwtac9vyp
```

For each address in the above list, construct a fake issuance transaction with an output
paying to that address (any nonzero amount, height >= 12642645). Call
`serializeMetaProcessor()`. Assert the result is NOT `""`.

The two broken addresses will return `""` until Bug 5 is fixed.

**Note:** `serializeMetaProcessor()` calls `tx.getIssuedAsset().getAssetId()` and
`db->getAcceptedExchangeRate()`, so you will need a minimal AppMain setup (at minimum a
database with an exchange rate row). Look at how `DigiAssetTransactionTest` sets up AppMain,
or consider refactoring to make this testable without a full DB.

---

**Test 4 — `mctrivia_updateBadList_noDuplicates`**

Detection test for Bug 6 (duplicate push).

Since `updateBadList()` fetches from `https://ipfs.digiassetx.com/bad.json`, this test
needs either:
- A mock/stub for `CurlHandler::get()`, OR
- Direct access to `_badAssets` via a test-only accessor

Simplest approach: use a test subclass that overrides `updateBadList()` to inject a
synthetic bad list, then call `isAssetBad()` to trigger the dedup path, then inspect
`_badAssets.size()`.

If mocking is impractical, at minimum add a `getBadAssetCount()` accessor and write an
integration test that calls `updateBadList()` twice and asserts the count did not double.

---

### Modifications to `tests/RPCMethods.cpp`

**Fix TearDownTestSuite (Bug 8):**

```cpp
void RPCMethodsTest::TearDownTestSuite() {
    ipfs->stop();        // stop IPFS thread BEFORE deleting anything
    appMain->reset();    // clear AppMain pointers before deleting objects
    delete dgb;
    delete ipfs;
    delete psp;
    delete db;
    delete analyzer;
    delete rpcCache;
}
```

---

### Modifications to `tests/DigiAssetTransactionTest.cpp`

Already done in this session:
- ✅ Reverted log level from `DEBUG` back to `INFO` (line 32)
- ✅ Added WAL/SHM removal in test setup
- ✅ Added `ipfs.stop()` before `main->reset()` at teardown

---

## RPC Tests: Pre-Existing Failures (Not Our Problem)

These 16 tests currently fail and were failing before any changes in this session:
```
RPCMethodsTest.getaddressholdings    RPCMethodsTest.getassetholders
RPCMethodsTest.getaddresskyc         RPCMethodsTest.getassetindexes
RPCMethodsTest.getassetdata          RPCMethodsTest.getdgbequivalent
RPCMethodsTest.getdomainaddress      RPCMethodsTest.getexchangerates
RPCMethodsTest.getpsp                RPCMethodsTest.getrawtransaction
RPCMethodsTest.gettxout              RPCMethodsTest.listaddresshistory
RPCMethodsTest.listassets            RPCMethodsTest.listlastassets
RPCMethodsTest.listlastassetspageindexes  RPCMethodsTest.listunspent
```

**Root cause:** These tests check for IPFS CIDs that must be pinned on the local IPFS node
(e.g. `getpsp` expects `results["files"][0] == "QmNMnCP97azWtKD4dV4VKavgoxkbpuF1LLa6wURyoUgMrY"`).
The local IPFS node doesn't have those files, so the result is `""`. These tests would
pass if the IPFS daemon had synced and pinned all the referenced content.

Do NOT attempt to fix these by mocking IPFS responses. They are integration tests by design.

---

## How to Build and Run Tests

```bash
# Build
cmake --build cmake-build-release --target Google_Tests_run -j4

# Run only tests that don't need IPFS (fast, always reliable)
cd bin
./Google_Tests_run --gtest_filter=-DigiAssetTransaction.existingAssetTransactions

# Run the big transaction test (needs IPFS daemon running, takes ~1-2 hours)
ipfs daemon &
./Google_Tests_run --gtest_filter=DigiAssetTransaction.existingAssetTransactions

# Run everything
./Google_Tests_run
```

Expected baseline (as of this session):
- 58 PASS (without DigiAssetTransaction)
- 16 FAIL (RPCMethodsTest — pre-existing IPFS content issue)
- DigiAssetTransaction.existingAssetTransactions: PASS (needs IPFS, takes time)

---

## Files Changed in This Session (Not Yet Committed)

```
src/PermanentStoragePool/PermanentStoragePool.h   (+virtual destructor)
src/PermanentStoragePool/pools/mctrivia.h         (+destructor declaration)
src/PermanentStoragePool/pools/mctrivia.cpp       (+destructor impl, interruptible sleep)
tests/DigiAssetTransactionTest.cpp                (log level, WAL cleanup, ipfs.stop())
```

Run `git diff HEAD` to see the exact diff.
