# Corrections & Gotchas

Things prior notes (`LAST_TASKS_NOTES.md`, `TODO_TESTS.md`) got wrong or that aren't obvious
from reading the code once. Read this first — it'll save time.

## 1. TODO_TESTS.md's bug table (5–8) is stale — already fixed and committed

`TODO_TESTS.md` lists bugs 5, 6, 7, 8 as "NOT FIXED" and says `tests/PermanentStoragePoolTest.cpp`
"does not yet exist." **Both are wrong as of the current commit** (`1627a91`). Verified directly:
- The missing comma (bug 5) — comma is present at `mctrivia.cpp:169-170`.
- The double-push (bug 6) — fixed, unconditional push removed.
- The data race (bug 7) — `std::mutex _badListMutex` exists and is locked.
- The teardown dangling pointers (bug 8) — `tests/RPCMethods.cpp:65-78` calls `ipfs->stop()`
  then `appMain->reset()` before any `delete`, matching the suggested fix.
- `tests/PermanentStoragePoolTest.cpp` exists, 196 lines, 4 tests, committed (not a stray
  working-tree file — `git status`/`git diff HEAD` on it are both clean).

**Do not re-fix these or re-write this test file.** Full verification detail in
`05_tests_and_psp.md`. Consider updating/deleting `TODO_TESTS.md` once you've confirmed this
yourself, so a future session doesn't hit the same stale claim.

## 2. `tests/testFiles/` is not a static fixture — don't assume it's pre-populated

`LAST_TASKS_NOTES.md` describes it as "a partial-chain test database exists for testing." In
reality the directory ships with only 2 tiny malformed-config fixtures. The real ~200K-tx test
database (`assetTest.db` → copied to `rpcTest.db`) is **downloaded from IPFS at test-run time**
by `DigiAssetTransactionTest.cpp`. If you run other tests (`RPCMethodsTest.*`,
`PermanentStoragePool.mctrivia_allAddressesRecognized`) without having run the transaction-replay
test first in that build dir, they'll fail with file-not-found errors — this is intentional
sequencing, not a bug. See `05_tests_and_psp.md` §"testFiles" for the exact code path.

## 3. "No live-chain testing until resync done" is broader than it needs to be

The transaction-replay test only calls `dgb.makeConnection()` — it needs `digibyted`'s RPC port
reachable, not a synced chain (all real tx data comes from the IPFS-downloaded DB, not live RPC
calls). Worth trying the test suite now, even mid-resync, rather than waiting. Smoke-test
`makeConnection()` behavior during IBD before fully relying on this — it's a reasonable bet, not
a 100%-verified guarantee. See `05_tests_and_psp.md` §"What this test actually requires."

## 4. Two build directories exist: `build/` vs `cmake-build-release/`

They have different cached CMake variable names in places (`BUILD_TEST` vs `BUILD_TESTS` was one
discrepancy noted) and different mtimes. `build/` is what `build_and_test.sh` (the Linux script)
creates fresh each time; `cmake-build-release/` looks CLion-managed. `TODO_TESTS.md`'s documented
build command (`cmake --build cmake-build-release --target Google_Tests_run`) may not match
whichever directory's binaries are actually current in `bin/`. **Check `ls -la` mtimes on both
before trusting either is up to date** — when in doubt, reconfigure fresh rather than assume
cache validity. Detail in `02_rpc_cli_and_build.md` and `05_tests_and_psp.md`.

## 5. Qt tabs require MANUAL registration — unlike RPC methods

RPC methods auto-register via a CMake glob (drop a `.cpp` in `src/RPC/Methods/`, rerun cmake
configure, done). **Qt tabs do not work this way** — a new tab class must be manually added to
`qt/CMakeLists.txt`'s source list AND manually wired into `qt/main.cpp`'s tab-mounting code
(exact insertion point: `main.cpp:148-157`). Forgetting either step means the tab silently isn't
built or silently isn't shown. Detail in `04_qt_gui_and_web_docs.md`.

## 6. `getrandom` RPC method is the one doc + test gap

- Has an `.html` doc file, but **no nav link** in `web/index.html` (task 5 gap).
- Has no `tests/RPC_Methods/getrandom.cpp` test file (task 4 gap) — but this one auto-globs, no
  CMakeLists.txt edit needed once the file is added, unlike most other test gaps.

## 7. Windows readme section is blocked on a branch merge decision

Windows/vcpkg build compatibility work already exists (commit `47568e1`) but only on the
unmerged `fast` branch, not `last_tasks`. Writing Windows build instructions into `readme.md`
without that code present would document an unsupported build path. Flag this to the user —
it's a scope/sequencing decision, not something to silently work around.

## 8. The DigiAsset v3 wire format does NOT need reverse-engineering

For tasks 1 & 2 (asset issuance/transfer encoding): the format is already fully documented in
doc-comments on the decode-side functions (`DigiAsset.cpp`, `DigiAssetRules.cpp`,
`DigiByteTransaction.cpp`'s decode functions), and `BitIO` already has matching write-side
primitives for every read primitive the decoder uses. This was flagged as a risk in
`LAST_TASKS_NOTES.md` ("must be written... using decoding as spec reference") but turns out to
be much less exploratory than that phrasing suggests — it's closer to "mirror the decoder using
existing helpers" than "invent an encoder from scratch." Detail in
`03_transaction_encoding_and_database.md` §2.
