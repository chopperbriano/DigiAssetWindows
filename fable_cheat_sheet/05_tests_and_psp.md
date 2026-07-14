# Tests & PermanentStoragePool (PSP) Module — Cheat Sheet

*For Task 4 ("finish test suite, all code tested") and general context on the PSP module.*

## ⚠️ CRITICAL: TODO_TESTS.md is STALE — bugs 5, 6, 7, 8 are ALREADY FIXED

`TODO_TESTS.md` (repo root) says bugs 5–8 are unfixed and that `tests/PermanentStoragePoolTest.cpp`
"does not yet exist." **Both claims are wrong as of the current commit** (`1627a91`, "Let claude
write more tests... also fixs some minor bugs"). Verified directly against source:

| Bug | TODO_TESTS.md claim | Actual state |
|---|---|---|
| 5 — missing comma in mctrivia address set | NOT FIXED | **Fixed.** `src/PermanentStoragePool/pools/mctrivia.cpp:169-170` has the comma. |
| 6 — `updateBadList()` double-push | NOT FIXED | **Fixed.** `mctrivia.cpp:281-287` — unconditional push_back removed, only pushes inside the `if` dedup guard now. |
| 7 — data race on `_badAssets`/`_badFiles` | NOT FIXED | **Fixed.** `mctrivia.h:29` has `std::mutex _badListMutex;`, locked at top of `updateBadList()` (`mctrivia.cpp:269`). |
| 8 — RPCMethods teardown dangling pointers | NOT FIXED | **Fixed.** `tests/RPCMethods.cpp:65-78` — `ipfs->stop()` then `appMain->reset()` now run before any `delete`. |

`tests/PermanentStoragePoolTest.cpp` exists (196 lines, 4 tests) and is git-tracked/committed —
not a stale working-tree file. **Do not re-fix these bugs or re-write these tests — verify first,
then move TODO_TESTS.md's bug table to "done" or delete the file to avoid future confusion.**

The only bug-5/6/7/8 test not yet fully exercised is `mctrivia_allAddressesRecognized` (see below —
it self-skips via `ASSERT_TRUE(fileExists(...))` failing loudly if `rpcTest.db` isn't present).

---

## `tests/` Directory Map

33 top-level test files + `tests/RPC_Methods/` (one file per RPC method, 33 files, glob-included).

| File | Covers | # TEST/TEST_F |
|---|---|---|
| `BitIO.cpp` | `src/BitIO.cpp` (bit-level stream read/write) | 43 |
| `UtilsTest.cpp` | `src/utils.cpp` | 25 |
| `BlobTest.cpp` | `src/Blob.cpp` | 23 |
| `DigiAssetTypesTest.cpp` | `src/DigiAssetTypes.cpp` | 21 |
| `DigiAssetRulesTest.cpp` | `src/DigiAssetRules.cpp` | 19 |
| `ConfigTest.cpp` | `src/Config.cpp` | 18 |
| `RPC_ResponseTest.cpp` | `src/RPC/Response.cpp` | 17 |
| `DatabaseTest.cpp` | `src/Database.cpp` | 16 |
| `Database_StatementTest.cpp` | `src/Database_Statement.cpp` | 12 |
| `UniqueTaskQueueTest.cpp` | `src/UniqueTaskQueue.cpp` | 11 |
| `RPC_CacheTest.cpp` | `src/RPC/Cache.cpp` | 11 |
| `LogTest.cpp` | `src/Log.cpp` | 11 |
| `ChainAnalyzerTest.cpp` | `src/ChainAnalyzer.cpp` | 10 |
| `SHA256Test.cpp` | `src/crypto/SHA256.cpp` | 9 |
| `KYCTest.cpp` | `src/KYC.cpp` | 8 |
| `DigiAssetTest.cpp` | `src/DigiAsset.cpp` | 8 |
| `ThreadedTest.cpp` | `src/Threaded.cpp` | 6 |
| `InstanceLockTest.cpp` | `src/InstanceLock.cpp` | 6 |
| `IPFS.cpp` | `src/IPFS.cpp` | 5 |
| `PermanentStoragePoolTest.cpp` | `mctrivia.cpp`, PSP base class (indirectly) | 4 |
| `PermanentStoragePoolMetaProcessorTest.cpp` | `PermanentStoragePoolMetaProcessor.cpp` | 4 |
| `CurlHandlerTest.cpp` | `src/CurlHandler.cpp` | 4 |
| `RPC_MethodListTest.cpp` | `src/RPC/MethodList.cpp` | 3 |
| `DigiByteCore.cpp` | `src/DigiByteCore.cpp` | 3 |
| `DatabaseChain.cpp` | `src/Database.cpp` (chain-specific paths) | 3 |
| `Base58Tests.cpp` | `src/Base58.cpp` | 2 |
| `DigiAssetTransactionTest.cpp` | `src/DigiByteTransaction.cpp` (decode path, historical replay) | 1 (but iterates ~198K historical txs inside it) |
| `RPCMethods.cpp`/`.h` | Shared fixture (`SetUpTestSuite`/`TearDownTestSuite`) used by `tests/RPC_Methods/*.cpp` | 0 (fixture only) |
| `TestHelpers.cpp`/`.h` | Shared test helpers | 0 (helper only) |
| `tests/RPC_Methods/*.cpp` (33 files) | One per `src/RPC/Methods/*.cpp` | varies |

### Gap list — source files with NO corresponding test file

Compared every `src/**/*.cpp` against the test list above:

- **`src/AppMain.cpp`** — no dedicated test. Heavily used as a fixture/singleton by other tests but its own logic (getters/setters, `reset()`) isn't directly unit-tested.
- **`src/DigiByteDomain.cpp`** — no test file found.
- **`src/OldStream.cpp`** — no test file found.
- **`src/PermanentStoragePool/PermanentStoragePoolList.cpp`** — no dedicated test (used as a fixture dependency in `DigiAssetTransactionTest.cpp` via `PermanentStoragePoolList psp("config.cfg")`, but its own methods — `getRandomPool`, `getPool`, `processNewMetaData`, the `_callbackNewMetadata` static — aren't directly tested).
- **`src/PermanentStoragePool/pools/local.cpp`** — the `local` pool implementation (`src/PermanentStoragePool/pools/local.h/.cpp`) has zero tests; only `mctrivia` (the other pool impl) is tested.
- **`src/RPC/Server.cpp`** — no dedicated `ServerTest.cpp`. Method-level behavior is tested via `tests/RPC_Methods/*.cpp` + the `RPCMethods` fixture, but request routing / unknown-method-forwarding logic in `Server.cpp` itself isn't isolated.
- **`src/Database_LockedStatement.cpp`** — `Database_StatementTest.cpp` covers `Database_Statement.cpp`; the separate `Database_LockedStatement.cpp` (RAII lock wrapper) doesn't appear to have its own dedicated cases — verify by grepping `Database_StatementTest.cpp` for `LockedStatement` before assuming a gap.
- **`src/main.cpp`** — expected to be untested (entry point / wiring only).

### RPC method test coverage: 1 gap

Diffed `src/RPC/Methods/*.cpp` (35 files) against `tests/RPC_Methods/*.cpp` (33 files, `glob`-included — see CMake section below):

**`getrandom.cpp` has no test file.** Every other RPC method has an exact 1:1 test file match
(e.g. `src/RPC/Methods/getpsp.cpp` ↔ `tests/RPC_Methods/getpsp.cpp`).

---

## `tests/testFiles/` — NOT a static fixture, it's a runtime download target

Contrary to what `LAST_TASKS_NOTES.md` implies ("a partial-chain test database exists for
testing"), **`tests/testFiles/` is checked into git with only 2 tiny files**:
`DigiByteCore_bad1.cfg` (78 bytes), `DigiByteCore_bad2.cfg` (34 bytes) — both just malformed
config fixtures for `ConfigTest.cpp`/`DigiByteCore.cpp` tests.

The actual "partial-chain database" is **downloaded from IPFS at test-run time** by
`tests/DigiAssetTransactionTest.cpp` (`DigiAssetTransaction.existingAssetTransactions`, the ~198K-tx
historical replay test):

```cpp
// tests/DigiAssetTransactionTest.cpp:69-77
IPFS ipfs("config.cfg", false);
try {
    ipfs.downloadFile("QmNPyr5tkm48cUu5iMbReiM8GN8AW6PRpzUztPFadaxC8j", "../tests/testFiles/assetTest.csv", true);
    ipfs.downloadFile("QmVoawgnYej8TNwpBB7DtJ75KbrAB99k7f9VAWzqSLJBeX", "../tests/testFiles/assetTest.db", true);
} catch (const IPFS::exceptionNoConnection&) {
    GTEST_SKIP() << "IPFS node not available - skipping transaction tests";
} catch (const IPFS::exceptionTimeout&) {
    GTEST_SKIP() << "IPFS node timed out - skipping transaction tests";
}
```

After the test runs, it copies `assetTest.db` → `rpcTest.db` (line ~323:
`utils::copyFile("../tests/testFiles/assetTest.db", "../tests/testFiles/rpcTest.db")`), then
deletes `assetTest.db`/`.csv`, **leaving `rpcTest.db` behind** for downstream consumers:

- `tests/RPCMethods.cpp` fixture (`SetUpTestSuite`) — feeds all `tests/RPC_Methods/*.cpp` tests
- `PermanentStoragePool.mctrivia_allAddressesRecognized` in `tests/PermanentStoragePoolTest.cpp`
  (self-guards: `ASSERT_TRUE(utils::fileExists("../tests/testFiles/rpcTest.db"))` — fails loudly,
  doesn't silently skip, if the file is missing)

**Practical implication:** `rpcTest.db` does not exist in a fresh checkout/build dir. You must run
`DigiAssetTransaction.existingAssetTransactions` first (needs a reachable IPFS node) before running
`RPCMethodsTest.*` or `PermanentStoragePool.mctrivia_allAddressesRecognized` — otherwise those all
fail with clear "file not found"-style errors, not silent skips (this is intentional, per the
warning comment at the top of `DigiAssetTransactionTest.cpp:5-18`).

### What this test actually requires — correcting the "no live-chain testing" assumption

`LAST_TASKS_NOTES.md` says "No live-chain testing until [DigiByte Core resync] done." Checked what
RPC calls this test actually makes against `dgb` (the `DigiByteCore` connection):

```cpp
// tests/DigiAssetTransactionTest.cpp:82-83 — the ONLY dgb. calls in the whole file
dgb.setFileName("config.cfg");
dgb.makeConnection();
```

That's it — **it only needs `digibyted`'s RPC server to be reachable, not a fully synced chain.**
All the actual transaction data comes from the pre-decoded `assetTest.db`/`.csv` downloaded from
IPFS, not from live `dgb` RPC calls. As of this session, `digibyted` is running locally (RPC on
port 14022, mid-resync) — RPC is reachable, so **this test can likely run now**, resync notwithstanding.
Confirm `makeConnection()` doesn't internally require `initialblockdownload: false` before assuming
this — worth a quick smoke-test.

---

## `tests/DigiAssetTest.cpp` — template pattern (per TODO_TESTS.md's suggestion)

Simpler than expected: plain `TEST(SuiteName, caseName)` macros (no fixture class, no
`SetUp`/`TearDown`), no `AppMain` construction needed for its cases — it tests pure/static
functions like `DigiAsset::calcSimpleScriptPubKey()` with hand-built structs:

```cpp
// tests/DigiAssetTest.cpp:44-57 (abridged)
TEST(DigiAsset, calcSimpleScriptPubKey) {
    vin_t test1;
    test1.txid = "...";
    test1.n = 0;
    test1.scriptSig = { .assm = "...", .hex = "..." };
    test1.txinwitness = { "..." };
    vector<uint8_t> test1Result = DigiAsset::calcSimpleScriptPubKey(test1);
    EXPECT_EQ(uint8_vector_to_hex_string(test1Result), "a9143ee55b86d113278c234a5e6064dd2f997de45d7587");
}
```

**Note:** `tests/PermanentStoragePoolTest.cpp` (already written, see above) is actually a *better*
template than `DigiAssetTest.cpp` for anything needing `AppMain`/`Database` setup — it demonstrates
the full pattern:

```cpp
// tests/PermanentStoragePoolTest.cpp:88-90, 133
AppMain* appMain = AppMain::GetInstance();
Database db("../tests/testFiles/rpcTest.db");
appMain->setDatabase(&db);
// ... test body ...
appMain->reset();   // always reset AppMain at the end, mirrors RPCMethods.cpp fixture pattern
```

If Fable needs to write more `AppMain`-dependent tests, copy this pattern (or the fuller
`tests/RPCMethods.cpp` `SetUpTestSuite`/`TearDownTestSuite` fixture for anything needing
`DigiByteCore` + `IPFS` + `PermanentStoragePoolList` + `RPC::Cache` all wired up at once).

---

## `src/PermanentStoragePool/` Module — conceptual overview

This module implements DigiAsset's **Permanent Storage Pool (PSP)** system: a mechanism where
node operators can subscribe to a "pool," pay to keep specific IPFS content pinned long-term, and
the codebase tracks which assets/files are healthy vs "bad" (unavailable/abandoned).

**Files** (1201 total lines):

- `PermanentStoragePool.h`/`.cpp` (108+135 lines) — abstract base class. Key virtuals:
  `serializeMetaProcessor(tx)` (called by ChainAnalyzer — returns non-empty string if `tx` is a
  payment to this pool), `deserializeMetaProcessor(data)` (builds a processor object for pinning
  decisions), `start()`/`stop()` (lifecycle for any background keep-alive thread).
  `virtual ~PermanentStoragePool() = default;` at line 30 — confirms Bug 1 (missing virtual
  destructor → SIGILL) is fixed.
- `PermanentStoragePoolList.h`/`.cpp` (49+226 lines) — owns a `vector<unique_ptr<PermanentStoragePool>>`,
  loaded from config, one instance per configured pool. `processNewMetaData()` is the entry point
  ChainAnalyzer calls when new asset metadata needs pool routing. Iterable (has `begin()`/`end()`).
- `PermanentStoragePoolMetaProcessor.h`/`.cpp` (23+27 lines) — small interface for "what to do with
  metadata once verified part of a pool" (pinning decisions).
- `pools/mctrivia.h`/`.cpp` (76+330 lines) — one concrete pool implementation. Talks to a remote
  server (`https://ipfs.digiassetx.com/...`) via `CurlHandler`. Has a background keep-alive thread
  (`keepAliveTask()`) that pings the server and periodically calls `updateBadList()` to refresh
  `_badAssets`/`_badFiles` (protected by `_badListMutex` — see bug 7 fix above). Recognizes payments
  to a hardcoded set of ~14 pool addresses in `serializeMetaProcessor()` (the bug-5 address set).
- `pools/local.h`/`.cpp` (60+167 lines) — the OTHER concrete pool implementation: a fully local pool
  backed by its own sqlite db (`local.db`, see `PSP_LOCAL_DB_FILENAME` in `local.h:9`). Has its own
  prepared statements (`_stmtCheckIfPartOfPool`, `_stmtMarkBad`, etc.) — **zero test coverage, noted
  in the gap list above.**

### Confirmed fix state via `git diff HEAD -- src/PermanentStoragePool/`

`git diff HEAD` on this path is **empty** — meaning bugs 1–7 are not "uncommitted working-tree
fixes still pending," they are already part of the last commit (`1627a91`) on this branch. Nothing
outstanding here; the diff only shows uncommitted changes elsewhere (`src/RPC/Methods/getrawtransaction.cpp`,
`tests/DigiAssetTransactionTest.cpp` — see `LAST_TASKS_NOTES.md`'s "Inherited uncommitted changes"
section, unrelated to PSP).

---

## CMake Test Target

Confirmed target name and file-inclusion mechanism in `tests/CMakeLists.txt`:

```cmake
# tests/CMakeLists.txt:19-53 (abridged)
set(TESTS
        BitIO.cpp
        DatabaseChain.cpp
        ... # 24 more explicit filenames ...
        TestHelpers.cpp TestHelpers.h RPCMethods.cpp RPCMethods.h)

#load RPC tests
file(GLOB RPC_METHOD_TEST_SOURCES RPC_Methods/*.cpp)
foreach(SOURCE_FILE ${RPC_METHOD_TEST_SOURCES})
    list(APPEND TESTS ${SOURCE_FILE})
endforeach()

add_executable(Google_Tests_run ${TESTS})
```

**Two different inclusion mechanisms — matters for adding new test files:**

1. **`tests/RPC_Methods/*.cpp` is auto-globbed.** Adding `tests/RPC_Methods/getrandom.cpp` (to
   close the one RPC-method test gap) needs **no CMakeLists.txt edit** — just create the file and
   re-run cmake configure (glob is evaluated at configure time, not build time, so a fresh `cmake`
   invocation — or touching CMakeLists.txt to force reconfigure — is needed after adding the file).
2. **Every other top-level test file is an explicit list (line 19-47).** Any NEW test file for the
   gap list above (e.g. `AppMainTest.cpp`, `DigiByteDomainTest.cpp`, `OldStreamTest.cpp`,
   `PermanentStoragePoolListTest.cpp`, a `local.cpp` pool test, `ServerTest.cpp`) **must be added
   by hand to the `set(TESTS ...)` block**, or it silently won't be compiled/run.

Build command (confirmed matches `TODO_TESTS.md`'s documented command):
```bash
cmake --build cmake-build-release --target Google_Tests_run -j4
```
(Also works against the `build/` dir per `build_and_test.sh`, which configures a fresh `build/`
tree from scratch with `-DBUILD_TEST=ON` — the two build directories are for different workflows:
`cmake-build-release` looks CLion-managed/pre-existing, `build/` is what the Linux shell script
creates. Verify which one is "live"/being actively used before building — check `ls -la` mtimes on
both if unsure.)
