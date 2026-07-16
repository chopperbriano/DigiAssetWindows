# mctrivia/development Integration — Change Record

**Read this before merging `development/mctrivia-integration` into `master`.**
It records everything the upstream merge changed, every decision made to keep the
Windows fork working, and the items you should review first.

| | |
|---|---|
| Branch | `development/mctrivia-integration` (off `master` @ `9f6ed0e`) |
| Merged | `mctrivia/development` @ `1627a91` (2026-04-22) — **117 upstream commits** |
| Merge commit | `d4443f4` |
| Safety anchor | tag `pre-mctrivia-integration` @ `9f6ed0e` (your pre-merge `master`) |
| Scope | 83 files changed, +5690 / −219; 18 conflicts, all resolved |
| Build | node + CLI + pool + unit-tests **all build clean** (MSVC Release) |
| Tests | **63 / 63 unit tests pass** (identical to pre-merge baseline) |
| Pushed to | `chopperbriano` only. `mctrivia`/`upstream` push URLs are **disabled**. |
| Merged to master? | **No.** Do not, until you've reviewed §5. |

---

## 1. Your Windows work is preserved

None of your fork-specific work was removed. Verified intact and untouched by the merge:

- **All PowerShell tooling** — `setup-digiasset.ps1`, `setup-pool.ps1`, `node/*`, `pool/deploy/*` (Caddy, provision-peer-pool, verify-peers, start-digistamp…), `snapshots/*`. Upstream doesn't have these files, so they never conflicted.
- **Web Console** — `web/index.html`, the 166 `web/rpc/*.html` docs, and the `GET /api/status.json` endpoint in `src/WebServer.cpp` (5 refs preserved). `WebServer.cpp` did not conflict.
- **Pool server** — `pool/` (DigiAssetPoolServer) builds and is unchanged.
- **Build/release layout** — binaries still land in `build/<sub>/Release/` (I deliberately did **not** adopt upstream's `bin/` redirect), so `update-binaries.ps1`, the release commands, and the snapshot scripts keep working.
- Every Windows adaptation in the C++ core (WinHTTP curl stubs, the boost sub-header includes that dodge the no-op stub, static-lib linking, chain.db self-heal, the RPC loopback bind + logging, `getnodestats`, permanent-coverage/health signals) was kept — see §4.

---

## 2. What the upstream merge brought in

- **`DigiAssetConstants` namespace** — upstream moved `standardExchangeRates` / `standardVoteAddresses` out of the `DigiAsset` class into `src/DigiAssetConstants.h` (values identical).
- **`InstanceLock`** — a single-instance guard (`src/InstanceLock.*`), wired into `main.cpp`.
- **`DigiByteCore::coreVersion()` / `WalletVersion`** — detects DigiByte Core 7.x vs 8.x and picks the matching bootstrap image; plus a `urlEncode()` fix for RPC credentials containing `/ + = @`.
- **SQLite WAL mode + a dedicated checkpoint connection** (`_dbCheckpoint`) in `Database`.
- **RPC server** bug-fix: bind before spawning worker threads (no thread leak on bind failure).
- **Qt GUI** (`qt/`, 8 files) — new; **not built** on this fork (see §5).
- **~35 new test files** and a `bin/` output layout — see §5/§6.

---

## 3. Conflict resolutions (18 files)

Legend: **ours** = Windows fork (`HEAD`), **theirs** = `mctrivia/development`.

| File | Resolution |
|---|---|
| `CMakeLists.txt` | Enable CLI+WEB; **BUILD_QT OFF**; keep `WIN_BUILD`/version, MSVC flags, pool + qt subdirs; **skip upstream `bin/` output redirect**. |
| `cli/CMakeLists.txt` | Keep `DigiAssetWindows-cli` target + winhttp; **add upstream's new `utils.cpp` dep**; drop upstream Linux pkg-config block (breaks MSVC configure). |
| `src/CMakeLists.txt` | Keep Windows static-lib linking; **add `InstanceLock.cpp`**. |
| `tests/CMakeLists.txt` | Keep Windows static-lib linking + `Unit_Tests_run`; guard upstream `find_package(OpenSSL/Threads REQUIRED)` to non-MSVC. |
| `.gitignore` | Union of both. |
| `src/RPC/Server.{h,cpp}` | Keep our loopback bind + logging + `_workGuard`; **adopt their bind-before-spawn fix**. Kept our `rpcthreads` config key (see §5). |
| `src/Database.{h,cpp}` | Keep our UTXO cache + self-heal; **adopt their WAL + `_dbCheckpoint`**; adapted write-verification pragmas to coexist (see §5). |
| `src/DigiByteCore.{h,cpp}` | Keep our prefetch pipeline + `PrefetchedBlock`; **adopt `WalletVersion`/`coreVersion` + urlEncode fix**. |
| `src/ChainAnalyzer.cpp` | Keep our pipeline/prefetch sync; **did not adopt their `walCheckpoint()`** (see §5). |
| `src/DigiAsset.cpp`, `src/DigiAssetRules.cpp` | **Adopt `DigiAssetConstants` namespace**; keep our audit bounds-checks. |
| `src/main.cpp` | Keep our AppMain wiring, ConsoleDashboard, self-heal, WebServer; **adopt InstanceLock + walletVersion bootstrap**. Signal-handler ordering per §5. |
| `src/DigiByteTransaction.h` | Keep our `toJSON`; add their test helpers. |
| `src/PermanentStoragePool/pools/mctrivia.{h,cpp}` | Keep our Health API + fetcher; adopt their bad-list refactor + destructor; kept our (retired) on-chain payout path (see §5). |

No blanket ours/theirs was used; every hunk was combined where both mattered.

---

## 4. Windows build fixes the merge required

1. **`src/InstanceLock.{cpp,h}` ported to Windows.** Upstream's version is POSIX-only (`flock`, `/proc/self/exe`, `kill`). Windows now uses a **named mutex** (OS-released on exit/crash, and it installs **no** signal handlers, so it doesn't fight the node's graceful shutdown). POSIX branch preserved for Linux.
2. **C++20 on MSVC** (root + `src/` + `cli/` CMakeLists). Upstream uses designated initializers, which MSVC accepts only at C++20 (gcc/clang take them as an extension). Added `/Zc:char8_t-` so C++20's `char8_t` change doesn't break `u8""` string code. Your tests already built at MSVC C++20, so this was low-risk — and it's verified by the clean build + 63/63.
3. **`main.cpp`:** `getpid` → `_getpid` on MSVC (`<process.h>`).
4. **`tests/CMakeLists`:** guarded upstream's `REQUIRED` OpenSSL/Threads finds to non-MSVC.

---

## 5. ⚠️ Decisions to REVIEW before merging to master

These are deliberate calls where upstream and the fork diverged. None break the build; each is reversible.

1. ~~**`walCheckpoint()` not integrated.**~~ **DONE (`ChainAnalyzer.cpp`).** Confirmed upstream *disables* SQLite auto-checkpoint (`Database.cpp:711`), so the WAL would grow unbounded during sync. Now wired safely: `db->walCheckpoint()` is called every ~2500 blocks **only when no header-batch transaction is open (`insertBatch == 0`)** so TRUNCATE can fully reset the WAL, plus a final flush when the analyzer reaches the tip. The checkpoint runs on the dedicated `_dbCheckpoint` connection (PASSIVE/TRUNCATE), so it never touches the main connection's cursors. **Still worth a real-resync sanity check** on WAL size + sync speed.
2. **Write-verification pragmas adapted (`Database.cpp`).** Our fast-catch-up `disableWriteVerification()` dropped `PRAGMA locking_mode=EXCLUSIVE` (would lock out the new checkpoint connection) and `enableWriteVerification()` now reverts to `journal_mode=WAL` (not `DELETE`). **Confirm** fast-catch-up sync speed is still acceptable on a real resync.
3. **RPC pool key kept as `rpcthreads` (default 16), not upstream's `rpcparallel` (8).** Ours is documented in `example.cfg` + `CHANGELOG.md`. Switching would be a user-facing config regression.
4. **On-chain payout path kept retired (`mctrivia.cpp`).** Upstream still implements on-chain fee-matching; our fork routes payouts through the pool server (`serializeMetaProcessor` returns `""`). Kept ours.
5. **Startup order (`main.cpp`):** the IPFS bootstrap download now happens **after** connecting to DigiByte Core (needed for upstream's `walletVersion`-based image selection). Signal handlers register **after** `InstanceLock::acquire()` so our graceful shutdown wins (moot on Windows — the named-mutex lock installs none).

---

## 6. Not integrated / deferred

- **Upstream's `Google_Tests_run` suite** now **compiles on Windows** (fixed `ConfigTest.cpp`'s POSIX `mkstemp` → portable `std::filesystem`). Its pure-unit tests pass (e.g. **18/18 Config tests**), but its **integration tests need a live DigiByte Core + IPFS** ("Core offline"/"IPFS down") — run them against a synced node. The no-services baseline remains **`Unit_Tests_run` (63/63)**.
- **Qt GUI (`qt/`)** — `BUILD_QT OFF`; not built (no Qt toolchain wired on this box).
- **Standalone web exe (`web/`, `digiasset_core-web`)** — `BUILD_WEB OFF`. Not needed: your Web *Console* is static files served by the node's built-in `WebServer` (in `src/`, always compiled). Turning it off keeps a fresh configure from building an unneeded upstream exe.
- **`bin/` output layout** — not adopted (keeps your build/release scripts working).

---

## 7. How to verify

```powershell
# from repo root, on branch development/mctrivia-integration
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build build --config Release --target DigiAssetWindows DigiAssetWindows-cli DigiAssetPoolServer Unit_Tests_run
.\build\tests\Release\Unit_Tests_run.exe --gtest_brief=1   # expect: 63 tests PASSED
```

**Validated on the build box (no DigiByte Core needed):**
- All 4 targets build clean at MSVC C++20; **Unit_Tests_run 63/63**; upstream Config tests 18/18.
- Merged node **boots without crashing**, reads config, and handles no-Core gracefully.
- **InstanceLock Windows port works** — a 2nd launch is correctly rejected ("Another instance is already running").

**Runtime resync test — PASSED (2026-07-16).** Ran `node/test-integration.ps1`
against a live Core:
- **Fresh sync from genesis:** synced 110 -> 24,026 blocks in ~2.5 min; WAL grew
  to 1.7 MB then repeatedly checkpointed back down; console + RPC answered.
- **Seeded with a real 1.2 GB production chain.db** (`-SeedChainDb`): the merged
  node **loaded it with NO rebuild** (schema unchanged - confirms existing nodes
  update cleanly), built the asset-era performance indexes, and **checkpointed the
  WAL from 8.2 MB down to 0.15 MB**.
- **No IPFS:** handled gracefully (logs "IPFS down", keeps running).

WAL fix: the checkpoint now fires every ~2500 blocks **OR every 60s** (in both the
sync loop and the idle-at-tip wait) - the seed test exposed that a per-block-only
trigger never fired while idle. Now bounded in all states.

Finding: on a Core with **multiple wallets loaded**, payout auto-create fails
(wallet RPC needs a `/wallet/<name>` path) - set explicit `psp0payout` +
`psp1payout` (pre-existing fork behavior, not the merge).

**Not yet exercised:** forward asset-*block* processing (processTX on new asset
blocks) - this box's Core is ~26k blocks behind the seed, so the node idled at the
seed height rather than processing new asset blocks. The asset-era DB/index path
and the asset logic (unit tests) are covered; a node whose Core is at/ahead of its
chain.db will process forward normally.

**Runtime resync test (run on a TEST server with DigiByte Core synced, NOT your production node box):**
```powershell
# on the integration branch, on a box where the PRODUCTION node is NOT running:
powershell -ExecutionPolicy Bypass -File .\node\test-integration.ps1 -WatchMinutes 20
```
`node/test-integration.ps1` is **isolated and safe** — fresh chain.db in its own
folder (`C:\DigiAssetIntegrationTest`), its own high ports (web 8922 / asset RPC
14924), reads DigiByte Core RPC read-only, never touches your production node,
wallet, chain.db, or snapshot. It watches sync progress + **WAL file size** and
prints PASS/FAIL for: node stays up, sync advances, **WAL stays bounded (proves
the checkpoint fix)**, console answers, CLI answers. Run it long enough to sync
past ~2500 blocks to see a checkpoint fire.

---

## 8. Merging to master — guidance

- **Do not** merge until you've reviewed §5 and are satisfied.
- Recommended: run a **real resync + runtime smoke test** on a test box first (the unit tests don't exercise sync/WAL/bootstrap end-to-end).
- If anything regresses, the anchor tag `pre-mctrivia-integration` is your exact pre-merge `master`.
- When ready: `git checkout master && git merge --no-ff development/mctrivia-integration` (on `chopperbriano` only).
