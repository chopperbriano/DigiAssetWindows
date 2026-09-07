# Changelog — DigiAsset for Windows

## Overview

DigiAsset for Windows is a Windows port of
[DigiAsset Core](https://github.com/DigiAsset-Core/DigiAsset_Core) by
[mctrivia](https://github.com/mctrivia) and contributors.
All core logic, chain analysis, RPC methods, and DigiAsset protocol
implementation are their work. This port adds only Windows build support,
platform stubs, a console dashboard, and sync optimizations.

Built with MSVC 2022 (x64). The original codebase assumed Linux system
libraries (libcurl, OpenSSL, libjsonrpccpp, SQLite3). This port replaces each
dependency with either a Windows-native implementation or a locally-vendored
source copy, so the project builds and runs without any external `vcpkg` or
system packages beyond a standard Visual Studio 2022 installation plus the
Boost NuGet package.

Version format: `{upstream_version}-win.{build}` (e.g. `0.3.0-win.4`)

---

## 0.3.3-win.138 (current) — the documented build works from a clean checkout again

win.137 removed a CI step on the reasoning that the local build did not need it. That
reasoning was wrong, and CI caught it: `libjson-rpc-cpp` genuinely does require CURL as
shipped. The local build only appeared not to need it because this machine still had
`libjson-rpc-cpp/install/*.lib` sitting there from **March** — five-month-old artifacts.
A fresh clone could not have built this project.

Two separate blockers, both now fixed in `patches/libjson-rpc-cpp-cmp0042.patch`, which is
applied by `config-libjson-rpc.bat` and by CI:

- `cmake_minimum_required(VERSION 3.0)` — CMake 4.x dropped compatibility below 3.5 and
  refuses to parse the file at all. This is why the failure looked different locally
  (CMake 4.2) than on the runner (CMake 3.31), which still accepts 3.0 with a warning.
- `cmake_policy(SET CMP0042 OLD)` — rejected outright by CMake 4.x. Already patched in
  win.133; the patch now covers both lines.

The CURL requirement itself is removed rather than satisfied: the submodule is configured
with `-DHTTP_CLIENT=NO -DHTTP_SERVER=NO`. Those are the only parts of it that link libcurl,
and this fork does not use them — it compiles its own connector,
`src/jsonrpccpp/client/connectors/httpclient.cpp`, backed by WinHTTP. Installing curl to
build a component we then replace was never the right answer, which is also why the old
`vcpkg install curl openssl` step was slow and fragile.

Verified end to end rather than assumed: `libjson-rpc-cpp/build` and `install` were deleted,
the dependency reconfigured and rebuilt from nothing with no curl anywhere, and the whole
project rebuilt with `--clean-first` against it. All 10 targets build, **103/103 unit tests
pass**, and CFG plus zero absolute paths still hold on all three binaries.

---

## 0.3.3-win.137 — second upstream sync; IPFS bootstrap dropped; asset rules enforced

Two upstream syncs' worth of work, plus the fallout from testing it. Entries for
win.130–136 follow below; this one covers the September sync.

### Merged `upstream/experimental_digidollar` (12 commits, 11 conflicts)

**The IPFS bootstrap image is gone, and this fork benefits more than upstream does.**
`main.cpp` pinned `officialBootstrap` **unconditionally on every start** — both the v7 and
v8 CIDs — whether or not the node ever restored from one. Every node this project has ever
shipped has been carrying several GB it never used. The retired CIDs move into
`oldBootstrapCIDs`, which is unpinned on start, so existing nodes **release that space on
their next run**.

Taking it costs nothing here: `--bootgen` and `Database::compactForDistribution()` existed to
build a single-file image, but this fork's fast-sync ships `chain.db` **with** its `-wal`/`-shm`
from a cleanly stopped node (`snapshots/make-snapshot.ps1`) and never used either.
`bootstrapchainstate` is gone from `example.cfg`; an existing config that still has the key is
simply ignored. Roughly everyone installs via `setup-digiasset.ps1` and the R2 snapshot, so
the IPFS fallback was paying a permanent cost for a path almost nobody took.

**A rewind that should never have happened.** `setDigiDollarSyncHeight` is now recorded when
the *normal sync path* passes the activation block, not only by `phaseDigiDollarBackfill`. A
database built by syncing **forward** claimed height 0, so the next start rewound ~212,000
blocks to redo finished work. That matters specifically here: the fast-sync snapshot ships a
prebuilt `chain.db`, so without this every node restoring from a snapshot would do that rewind
once, for nothing.

**A `std::terminate` hazard.** `Threaded::start()`/`stop()` only joined when `_running` was
set. A thread that ended on its own is still joinable, and destroying or assigning over one of
those ends the process. Both now join whenever there is something to join.

Also adopted: a stall watchdog in `ChainAnalyzer` that names the step a hung pass is waiting
on; IPFS and pool-server timeouts (a wedged daemon used to stop the analyzer on whichever
block held the next issuance); throttled IPFS warnings; and a v9 wallet requirement.

Kept over upstream where this fork diverges deliberately: `std::atomic` rather than upstream's
`volatile` flags; the **startup retry** (upstream logs CRITICAL and ends the thread — this fork
backs off and recovers, which is the win.130 fix for a node that sat dead for 4.6 hours); the
height-based error streak; the pipeline-prefetch `phaseSync`; DigiStamp pool defaults; keepalive
diagnostics; and the dashboard/WebServer/PSP shutdown ordering.

Two things the merge itself broke, both caught by building:

- `_reportResult` calls arrived without their declaration. It had been judged dead code because
  this fork's lambda already catches everything — that was wrong: `get()` also **retires** the
  future, which the previous `wait()`/erase did not.
- `CURLOPT_CONNECTTIMEOUT` does not exist in this fork's WinHTTP curl stub. Added and mapped
  onto `WinHttpSetTimeouts`' resolve+connect, so an unreachable host fails in ~10s instead of
  holding the thread for the full request timeout — 20 minutes on a pin, which was the entire
  point of upstream's change.

### Asset rule enforcement (upstream `62420c4`, `92dae26`)

A transfer of an asset whose rules a wallet-built transaction can never satisfy is now refused
before any input is chosen. Previously it was built and broadcast, failed `checkRulesPass` when
decoded, and the failure handler cleared the asset from **every** output including the change —
so sending 1 of 5 units silently destroyed all 5.

This fork had its own version and upstream credits it for the approach, but upstream's is a
superset, so **ours was removed rather than kept alongside it** (both defined
`assertTransferableAsset`; the tree would not have compiled). Upstream's adds what this fork got
wrong: our comment grouped **expiry** with KYC and vote rules as "not rejected here, because a
compliant transfer is legal." True for KYC and vote — the recipient decides — but not for
expiry, which is absolute: an expired asset can never pass validation again, so building the
transfer only destroys what is left.

It also takes chain height and time as arguments so the logic is testable without a database,
and logs a WARNING on the burn path, which previously destroyed every asset output in a
transaction with nothing written anywhere.

The nine tests arrived attached to `Google_Tests_run`, which needs a live DigiByte Core and
IPFS and so does not run on a normal build. They use the injected-state overload precisely so
they need neither, and were moved to `Unit_Tests_run` — a test that never runs is not coverage.

**Not taken:** upstream `bfef1ab` and `87b1c3a`. Both are the Linux release pipeline — a
workflow building `.deb`/`tar.gz` plus the Qt GUI, two Linux `.desktop` entries, and
`qt/CMakeLists.txt`. This fork ships Windows binaries, releases them by hand, and has
`BUILD_QT OFF` with no Qt toolchain wired.

### CI

`.github/workflows/release.yml` **removed**. It built Linux, macOS and Windows artifacts on
every `v*` tag and **had never once succeeded in 20 runs**, so every release this project cut
left a red X on the repository. It also could not produce what is actually shipped. The release
process is now documented in [docs/releasing.md](docs/releasing.md).

`windows-build.yml` fixed on two counts:

- It configured `libjson-rpc-cpp` directly and so never applied
  `patches/libjson-rpc-cpp-cmp0042.patch`, which a CMake 4.x build requires — a gap left when
  that fix moved into `config-libjson-rpc.bat` in win.133. It now applies it the same
  idempotent way.
- The `vcpkg install curl openssl` step is **gone**. It failed on every run this workflow ever
  had and was never required: `config-libjson-rpc.bat` — the documented local build, which
  works — configures that submodule with no vcpkg toolchain and no curl or openssl at all.

**103/103 unit tests pass** (was 94). Control Flow Guard, zero absolute build paths, and
published checksums all re-verified on the shipped binaries.

---

## 0.3.3-win.136 — an indented comment in config.cfg no longer stops the node starting

`Config` treated only a `#` in **column 0** as a comment, so an indented line parsed as a key:

```
   # psp2costpercent=100
```

Harmless until the config placeholder check arrived in win.134. That key contains a `#`, so
`main.cpp` logged CRITICAL and returned 1 — **the node refused to start** — with a message
describing the opposite of what happened: *"…is not a real config key … as written it does
nothing."* It was not doing nothing; it was preventing startup entirely. Indenting a comment is
an ordinary thing to do when hand-editing a config.

A comment is now any line whose first non-whitespace character is `#`; whitespace-only lines are
skipped too. Raw lines are still preserved, so comments survive write-back unchanged.

Verified against the built binary, not just the parser: the shipped `example.cfg` starts, an
indented comment containing `=` now starts, and a genuine `psp#subscribe=1` is still refused —
the check still catches the mistake it exists for. Two regression tests cover both directions.

Found by testing what a deployment would actually do rather than reading the diff.

---

## 0.3.3-win.135 — re-runnable 6→7 migration + migration coverage

win.134 removed a duplicated 6→7 migration lambda that the upstream merge introduced. That bug
got as far as it did because **nothing covered the migration path at all**.

Every statement in the 6→7 migration is now `IF NOT EXISTS` / `INSERT OR IGNORE`, matching the
fresh-create path, so a node killed part way through heals on the next start instead of erroring
forever on tables it already created.

`buildTables`' migration loop tested `dbVersionNumber >= skipUpToVersion` — a condition that
cannot change inside its own loop, because `skipUpToVersion` only ever moves inside
`lambdaFunctions[0]`, which only runs when `dbVersionNumber` is 0. It was correct, and it read
like a bug in the one function that had just produced a real one. Now tests `i`.

Adds `DatabaseMigrationTest` (5 cases): a fresh database lands on the current version, reopening
a current database does not re-migrate (**the actual regression**), repeated opens stay stable, a
version 6 database migrates forward and creates the DigiDollar tables, and an interrupted
migration is safe to re-run.

The tests were verified to have teeth: with the idempotency reverted,
`InterruptedMigrationIsRerunnable` fails while the other four still pass — the correct split,
since only that one exercises the re-run case.

`SHA256SUMS` is now written with **LF** endings. The win.134 file had CRLF, which made
`sha256sum -c` fail on every line while the hashes themselves were correct — and the release
notes tell people to run exactly that command.

---

## 0.3.3-win.134 — first upstream sync (18 commits) and the defects it introduced

Merged `upstream/experimental_digidollar` since 2026-07-22. Two upstream fixes **replaced this
fork's own attempts at the same bugs**:

- **Error classification.** win.130 fixed the "everything is Core Offline" problem the wrong
  way — it appended the real message to a still-incorrect label. Upstream checks the error
  *code*: only `ERROR_CLIENT_CONNECTOR` means unreachable, an auth failure is its own case, and
  anything else is rethrown with the node's own code and message. A healthy node answering
  "unknown method" is not a connectivity problem.
- **Sync no longer stops permanently.** win.132 gave up after 10 failures, set `STOPPED`, and
  slept until restarted — turning a long transient fault into a dead node. It now retries every
  15s. This fork's **height-based** streak reset is kept over upstream's error-text comparison,
  which counts unrelated hiccups spread over an hour as consecutive.

Also from upstream: `rpcwallet` for multi-wallet nodes; refusing to start on config keys still
holding the `#` placeholder; asset data on pruning nodes instead of an error; no longer locking
every fee coin when `storenonassetutxo=0`; a crash fix on duplicate exchange-rate publish in one
block; and `wakeBlockedAccept` — closing the acceptor does **not** unblock a thread already
inside `accept()`, which this fork's comment claimed it did.

**Four defects the merge itself introduced**, each found by building or testing:

| Defect | Impact |
|---|---|
| Duplicate `getNodeVersion` / `MINIMUM_NODE_VERSION` | Compile error |
| Duplicate DigiDollar `CREATE TABLE` block | Fresh database creation failed |
| Duplicate `_stmtReplaceExchangeRate.prepare()` | "Statement already prepared" |
| **Duplicate 6→7 migration lambda** | **Would have broken every existing node on upgrade** |

Both forks implemented DigiDollar indexing independently, so git kept both migrations. A
database already at version 7 ran the second and tried to `CREATE TABLE ddutxos` over itself.

Two further fixes the merge exposed: `Log` no longer depends on `ConsoleDashboard` (upstream
gives the cli `Log.cpp`, and pulling the dashboard in would drag `Database`/`AppMain`/`IPFS`
into a small CLI tool — it now takes a `std::function` sink); and the two migration paths threw
a bare `"Table creation failed"` while discarding the sqlite error unread, which is what made
the duplicate migration slow to find.

---

## 0.3.3-win.133 — hardened binaries and published checksums

These binaries are downloaded by the public, so this release audits what actually ships.

**Control Flow Guard was missing.** ASLR, DEP and high-entropy VA were already on as MSVC
defaults (`dumpbin`: DLL characteristics `0x8160`), but CFG is not a default and `0x4000` was
absent. Now compiled with `/guard:cf` and linked with `/GUARD:CF` — **both** are required or the
guard tables are silently never emitted. Verified `0x8160` → `0xC160` on all three binaries.
`/GS` is set explicitly so a future flag change cannot quietly drop stack cookies, and `/sdl`
adds the remaining checks.

**The exe was publishing the build layout.** It carried 15 absolute
`C:\repo\DigiAssetWindows\packages\boost...` strings, baked in by Boost's assert machinery via
`__FILE__`. `/d1trimfile:` strips the source root; those are now relative. No PDB path or
username was ever leaking. Now **0 absolute paths** in all three binaries.

**`SHA256SUMS` published with every release and verified on download.** Generated by
`tools/stage-release.ps1` so it cannot be forgotten — a checksum file published only when
someone remembers is worse than none, because the installer silently downgrades to a format
check and nobody notices. A **mismatch is fatal** and leaves the working binary untouched; a
**missing** file only warns, so rolling back still installs.

Three `std::getenv` calls were fixed rather than silenced (`/sdl` rejects it: it returns a
pointer into a shared buffer another thread can invalidate). `envFlagSet()` uses `_dupenv_s`.

`/Qspectre` is deliberately absent — it needs the Spectre-mitigated CRT, a separate Visual
Studio component every builder would have to install, for little benefit on a single-user
desktop node. `/GL` + `/LTCG` is also left off: a real speed win, but it re-optimises the whole
binary and the tests do not cover the sync path, so it wants its own release and a soak test.

---

## 0.3.3-win.132 — transient RPC hiccups no longer stop a healthy sync

A node doing the one-time DigiDollar backfill (a ~212,000 block replay) logged red CRITICALs
minutes apart, each recovered on the next pass. Chasing why they were CRITICAL turned up a worse
problem behind them.

`_errorCount` exists to catch **one block that keeps failing**, and is reset by the
`_errorCount = 0` after `phaseSync()`. But `phaseSync()` walks block by block and does not
return until it reaches the tip, so during a long catch-up that reset never runs. Unrelated
transient RPC failures — minutes apart, each recovered on the first attempt — accumulated into a
fake streak, and at 10 the node logged `Giving up`, set `STOPPED` and slept until restarted. The
box was at attempt 2 with ~55 minutes of replay left, on course to stop a sync that was making
steady progress at 64 blocks/sec. The counter now resets whenever the chain has advanced past
the previous failure.

The CRITICAL itself was pure noise. `mainFunction` re-threw with the comment *"so the Threaded
framework re-enters mainFunction() to recover"* — but Threaded's loop calls it again on the next
iteration whether or not it threw. The re-throw bought **no recovery at all**; its only effect
was reaching Threaded's catch-all, which logs everything at CRITICAL. Now reported at WARNING
with the real cause; the give-up path still logs CRITICAL.

---

## 0.3.3-win.131 — the pool server reports its build

`stats.json` gains a `version` field and the pool page footer shows it. Nothing exposed which
build `pool.digistamp.co` was running: after a deploy there was no way to confirm from outside
the box that the new binary had taken, and a peer pool on a stale build stayed invisible until
it misbehaved.

The footer shows **two** independent signals, because the site is pulled from `master` while the
binary ships in a release and the two deploy separately: **pool server**, read from `stats.json`
(proves the binary updated), and **page**, a stamp baked into `index.html` (proves the page
refreshed).

---

## 0.3.3-win.130 — a failed worker startup no longer kills the thread for good

A node was found sitting **3,194 blocks behind for 4.6 hours** while its dashboard showed
`DigiByte Core: Online`, `Initializing...` and a `100.0%` progress bar. The `ChainAnalyzer`
thread had been dead since 99 seconds after launch.

`Threaded::_threadFunction()` treated any `startupFunction()` exception as fatal: log one
CRITICAL, clear `_running`, return. Nothing ever restarted the worker. The analyzer calls
`getBlockHash()` while DigiByte Core may still be warming up, so losing that race killed
indexing for the life of the process while the rest of the node kept reporting itself healthy.

- Startup now **retries with backoff** (2s→60s) and honours `stop()`, so shutdown never waits
  out a backoff. Logging is graduated: WARNING per attempt, one CRITICAL on the third, INFO on
  recovery.
- The analyzer **waits for Core to reach the height it needs** rather than throwing at it, so
  the common warmup case never reaches the log.
- `disableWriteVerification()` moved after that wait — a startup that never completed used to
  leave `chain.db` in relaxed-durability mode with no sync running.
- The dashboard caps progress below 100% until the height actually reaches the tip (3,194 blocks
  behind out of 24 million is 99.99%, which printed as `100.0%`), and reports `INITIALIZING`
  past 120s as **STUCK** in red with the elapsed time.
- `Threaded::isRunning()` added so a watchdog can spot a dead worker.

Adds `ThreadedStartupTest`: retry after a transient failure, recovery once the dependency comes
up, staying alive while startup keeps failing, and `stop()` not hanging in the backoff.

---
## 0.3.3-win.105 — CRITICAL: fix win.104 crash (CurlHandler use-after-free)

win.104 crashed both the node (heap corruption, `0xc0000374`) and the pool
(access violation, `0xc0000005`) after running a while. Root cause: the
asset_features merge added `curl_easy_cleanup()` calls in `CurlHandler`'s `get`,
`post`, `getDownload`, `postDownload` — but those use a **reused thread-local
handle** (`tl_curl`). Cleaning it up without nulling `tl_curl` left a dangling
pointer that the next HTTP call reused via `curl_easy_reset()` → use-after-free.
`post()` did it on **every** call (success path too); the others on
timeout/abort. Both node and pool make constant HTTP calls, so both corrupted
their heap; the delay is because a POST or a timeout has to happen first.

Fix: a `discardHandle()` helper that cleans up **and nulls** `tl_curl`, used on
the error/timeout paths; successful transfers keep the handle for reuse (as
before win.104). Also freed a `postFile` handle leak. **Anyone on win.104 should
update immediately.**

---

## 0.3.3-win.104 — upstream asset_features (PR26) + chain-split regression tests

Folds the tested upstream **asset_features** work (DigiAsset Core's v1.0.0 branch,
PR #26) into master, plus the Windows fixes and new tests it needed:
- **New asset RPCs:** `issueasset`, `reissueasset`, `burnasset`, `sendasset`,
  `sendmanyassets`, `getwalletbalances`, `getnewaddress`.
- **TCP event stream** (`EventBroadcaster`, config `eventport`) broadcasting
  newBlock + asset-activity events; legacy `OldStream` removed.
- **Windows fixes:** guarded the POSIX InstanceLock fd under `#ifndef _WIN32`;
  implemented real WinHTTP **multipart `postFile`** (so `issueasset`-with-media
  uploads on Windows) — the stub curl only declared the mime API before.
- **Chain-split-2026 regression tests** (unit suite now **66/66**): asserts the
  Groestl block 23,751,096 (legacy version `0x00000400`) parses cleanly, and that
  scriptPubKey address extraction works for both v7 plural `addresses[]` and
  v8/8.22+ singular `address`. Fixtures in `tests/fixtures/chain-split-2026/`.

**chain.db unchanged** (dbVersion still 6) — existing nodes update with **no
resync**. Held at 0.3.3 (not 1.0.0): per upstream, a real 1.0.0 also needs the two
new PSPs, which are not done yet. Qt GUI not built on Windows (`BUILD_QT` off).

---

## 0.3.3-win.103 — clean RPC error messages (no double-wrapping)

`DigiByteException::parseMessage()` wrapped every thrown message in
`Error during parsing of >>…<<`, even plain human strings that RPC handlers
throw (e.g. `Domain Burned`). The CLI then re-parsed the response and wrapped it
again, producing the nested
`Error during parsing of >>Error during parsing of >>Domain Burned<<<<`.
`parseMessage()` now passes a non-JSON message through unchanged and only emits
the "Error during parsing of" diagnostic when the input actually looks like a
JSON payload (`{…`) that failed to parse. Also guards a latent `ret[0]` crash on
an empty message. Cleans up **all** RPC error messages, not just the domain one.

---

## 0.3.3-win.102 — accurate sync auto-recovery diagnostics

The auto-recovery log line could report **"block 0 … Cause: unknown error"**
when the exception was thrown from the recovery preamble (the
`getBlockHeight` / `getBlockHash` / `clearBlocksAboveHeight` calls) rather than
from `phaseSync` — e.g. a `Database Exception: SQL command failed`. That path
never set `_lastError`/`_lastErrorHeight`, so the real cause was lost and you had
to correlate it with the separate framework CRITICAL line. `mainFunction()` now
wraps its **entire** body in the capture try/catch (plus a `catch(...)` and a
`typeid` fallback for an empty `what()`), so every recovery names the **real
cause and a real block height**. No behavior change to recovery itself.

---

## 0.3.3-win.101 — getdomainaddress reports burned domains

`getdomainaddress` now reports a burned domain (registered, but its controlling
asset has no holders — swept by a non-DigiAsset-aware wallet) as **"Domain
Burned"**. Previously that case escaped to the generic handler and came back as
*"Unexpected Error,"* so callers couldn't tell a burned domain from an actual
node error/outage (RPC method + its HTML reference page updated). All targets
rebuilt at win.101.

---

## 0.3.3-win.100 — pool dashboard header fix

Fixes the pool server console dashboard scrolling its name/version header off
the top. `PoolDashboard::render()` hardcoded the header at 10 rows, but the
header actually emits ~13, so the log area overran the window by a few lines
each frame and the console scrolled. It now counts the actual header rows
emitted and sizes the log area from that (matching how the node's
`ConsoleDashboard` sizes itself), so the header stays pinned. Rebuilt all
targets at win.100.

---

## 0.3.3-win.99 — merge upstream mctrivia/development

Integrated 117 upstream commits from `mctrivia/development` (DigiAsset Core 0.3.3)
into the Windows fork, preserving all Windows functionality. Highlights: upstream
`DigiAssetConstants` refactor, single-instance guard (ported to a Windows named
mutex), DigiByte Core wallet-version detection + bootstrap selection, SQLite WAL
mode with a dedicated checkpoint connection (WAL checkpoint wired into the sync
loop + a 60s idle timer), and the RPC bind-before-spawn fix. Built at C++20 on
MSVC (upstream uses designated initializers). Fully validated: builds clean,
63/63 unit tests, live asset-era resync, and an existing production chain.db loads
with **no rebuild** (schema unchanged - existing nodes update cleanly). Full change
record + decisions in **[INTEGRATION-mctrivia.md](INTEGRATION-mctrivia.md)**.

---

## 0.3.0-win.98 — Node Console on :8090

The built-in web UI (http://localhost:8090) is no longer a static RPC doc dump —
it's a live **Node Console**, loopback-only.

- **New live endpoint** `GET /api/status.json` (`src/WebServer.cpp`) — reads the
  same null-safe subsystems the terminal dashboard uses: sync height + chain tip
  + progress, DigiAssets indexed + latest issuances, IPFS/bitswap serving stats,
  permanent-storage coverage, service health (Core/DB/IPFS/RPC/Web), external IP,
  uptime. Never cached; safe to poll.
- **New dashboard** (`web/index.html`) — a polished dark console with a live
  **Dashboard** tab (polls every 3s, client-side blocks/sec, offline banner) and
  an elegant, searchable **RPC Reference** tab with a "how to call these" primer
  (DigiAsset CLI @14024 vs Core CLI @14022), per-method interface hints, and
  copy-paste examples.
- **27 new method docs** — every DigiAssets RPC method
  (`listassets`, `getassetdata`, `syncstate`, `getexchangerates`, the `async*`
  trio, …) now has an accurate reference page generated from the actual source.
  Previously these were broken links upstream.
- **Full parity with the terminal dashboard** — pool reachability + online node
  count, hosting/payment status, payout address and balance now flow through the
  shared `NodeStats` singleton (written by the console's existing background
  checks) so the web console shows them too, plus a live ETA. New **Pool &
  Payouts** card.
- **Same look as pool.digistamp.co** — animated constellation + drifting aurora
  background, translucent cards, matching dark palette. Pauses when hidden and
  respects `prefers-reduced-motion`.
- **Full DigiByte node + wallet stats** — new **DigiByte Network** card (chain,
  peers connected, verification progress, difficulty, on-disk size, Core
  version) and **DigiByte Wallet** card (balance, unconfirmed, immature, tx
  count, encrypted/locked state) via a throttled Core RPC cache
  (`getblockchaininfo`/`getnetworkinfo`/`getwalletinfo`) in `/api/status.json`.
- **Web assets now ship to nodes** — `web.zip` is a release asset;
  `update-node.ps1` and `setup-digiasset.ps1` download + extract it beside the
  exe (the `web/` folder was never deployed before, so :8090 served nothing on a
  real node box).

## 0.3.0-win.90 through win.97

### Tooling (ships from master)
- `pool/deploy/provision-peer-pool.ps1` — all-in-one: turn a based box into a
  public pool + pair it with an existing pool in one command. Plus `add-peer.ps1`
  (wire to a peer), `verify-peers.ps1` (test the link, `-TestAnnounce`, self-test).

### win.97 — on-demand on-chain announce test
- New token-gated `POST /peer/testannounce` forces one on-chain announcement now
  (bypassing the weekly gate) and returns the txid, or the exact failing step
  (createrawtransaction / fundrawtransaction / sign / send). Run it via
  `verify-peers.ps1 -TestAnnounce` to validate the on-chain path on a live pool
  without waiting a week. `onchainAnnounce()` refactored to a forceable
  `doOnchainAnnounce()` returning a result string. Peer/discovery HTTP layer was
  also runtime-verified with two local instances this cycle.



### win.96 — on-chain pool discovery + network map/site
- **On-chain discovery (phase 2):** pools find each other with NO seed by
  announcing their URL in a DigiByte `OP_RETURN` (weekly, `DGSP1` magic) and
  scanning new blocks for others' announcements (forward-only from the tip).
  Still display-only + probe-validated. Config `poolonchain` (default 1); uses
  the pool wallet (`poolwalletpassphrase` to unlock) - skips gracefully if it
  can't fund/sign.
- **Site + map:** the landing-page world map now shows the WHOLE network - this
  pool's nodes (blue) plus peer/discovered pools' nodes (amber), with a "part of
  a network of N pools, M nodes worldwide" banner driven by `stats.json`'s
  `network.totalPools` / `network.directory`.



### win.95 — automatic pool discovery (seed + gossip, display-only)
- Pools now **auto-discover** each other over the network: a new pool announces
  its public URL to a seed (`poolseed`, defaults to the flagship) and gossips
  `GET /peer/list` until it has the whole directory. `stats.json` gains
  `network.totalPools` + `network.directory`, and all pools' nodes show on one
  map. **Display-only + untrusted** — discovered pools are NOT used for list
  mirroring or payout dedup (that stays gated to the explicit `poolpeers` +
  token). New open endpoints `GET /peer/list`, `POST /peer/announce`; config
  `poolpublicurl` (written by setup-caddy from -Domain), `poolseed`. See
  POOL-SETUP.md.



### win.94 — peer-aware independent pools
- Two (or more) independent pools (each own wallet + payouts) can now be **aware
  of each other**. New token-gated pool API — `GET /peer/status`, `/peer/ledger`,
  `/peer/assets` — and a background sync that: shows the **combined network** on
  the site/stats, **mirrors the permanent list** so both fleets pin the same
  content, **merges nodes onto one map** tagged by pool, and **coordinates
  payouts** so an operator served by both pools isn't paid twice in a period.
  Config: `poolpeers`, `poolpeertoken`, `poolpeerpayoutdedupe`. See POOL-SETUP.md.



Recent releases. Binaries are published on the
[Releases](https://github.com/chopperbriano/DigiAssetWindows/releases) page;
update with `update-node.ps1` (node), `update-binaries.ps1 -Force -IncludePool`
(pool), or `update-pool.ps1` (whole pool box).

### win.93 — clearer errors
- `CurlHandler::exceptionTimeout::what()` had the wrong signature so it never
  overrode `std::exception::what()`; a connection timeout logged the useless
  "Unknown exception" (MSVC default). Now reports "request timed out", so an
  unreachable pool reads clearly in the log.

### win.92 — timestamped + colorized logs
- Node + pool dashboards prefix each on-screen log line with the time
  (`MM-DD HH:MM:SS`); the node **log file** now carries a full
  `YYYY-MM-DD HH:MM:SS` timestamp (it had none).
- The pool log is now colorized by severity (red = FAIL/ERROR/CRITICAL,
  yellow = WARNING/TIMEOUT/locked, green = SENT), matching the node.

### win.91 — memory audit
- `_geoCache` in the pool no longer grows without bound (rebuilt each refresh to
  the active-node set).
- RPC cache size accounting now adjusts on overwrite so the 100 MB cap holds.

### win.90 — chain.db self-heal
- A torn/half-built `chain.db` (e.g. after a power loss during the relaxed-
  durability sync) no longer FATALs with `table "assets" already exists`.
  Idempotent schema build (`CREATE TABLE IF NOT EXISTS` / `INSERT OR IGNORE`), a
  `PRAGMA quick_check` + defensive version read that rebuilds an incoherent DB in
  place, and a `main.cpp` fallback that renames a corrupt file aside and rebuilds
  from scratch (chain.db is re-derivable) instead of dying.

### Tooling + docs (ship from `master`, not the binaries)
- **Node:** `update-node.ps1` (simple node-only updater), `memwatch.ps1` (leak
  detector). Installer (`setup-digiasset.ps1` v2.18.0) now offers wallet
  encryption, attempts UPnP port-forward, and asks full-vs-lean service node.
- **Pool/deploy:** `update-pool.ps1` (one-command pool-box update + live-site
  refresh), `diagnose-website.ps1` (why is Caddy down), `verify-pool-stack.ps1`
  (RPC/index/reindex-trap health check). `setup-caddy.ps1` now pins Caddy's cert
  store (fixes the SYSTEM-task "works by hand, dies as a task" bug) and removes
  the default 3-day task time limit; `start-digistamp.ps1` always restarts the
  website and exits cleanly.
- **Fleet:** `snapshots/snapshot-digibyte-datadir.ps1` provisions nodes from a
  built, indexed datadir over the LAN — no reindex, no re-sync.
- The pool landing page gained a visual "how it works" journey + an **animated
  network map**.

---

## 0.3.0-win.4

### Executable renamed
- Main executable renamed from `digiasset_core.exe` to `DigiAssetWindows.exe`
- CLI renamed from `digiasset_core-cli.exe` to `DigiAssetWindows-cli.exe`

### Integrated web server
- Web UI server (Boost Beast HTTP) is now built into the main executable —
  no need to run `digiasset_core-web.exe` separately
- Serves the web UI on configurable port (`webport`, default 8090)
- Dashboard displays web server status, clickable link, and external IP

### Console dashboard
- In-place TUI replaces scrolling log output (VT100 escape sequences)
- Fixed sections: header with version, services status, sync progress with
  speed/ETA/progress bar, asset count, and recent log messages
- Color-coded log messages by severity level

### Sync performance
- Parallel block prefetch pipeline (4 RPC workers with independent connections)
- In-memory non-asset UTXO cache eliminates RPC fallback during sync
- Thread-local CURL handle pooling (WinHTTP connection reuse)
- SQLite tuning: 256MB page cache, memory-mapped I/O, temp_store=MEMORY
- Pre-asset blocks (~1000 blocks/sec), asset blocks (~100-200 blocks/sec)

### Other improvements
- Configurable RPC thread pool size (`rpcthreads`, default 16)
- IPFS idle polling reduced from 500ms to 100ms
- `DigiAssetRules::operator==` made const (C++20 compatibility)

---

## 0.3.0-win.1 through win.3 (initial port)

---

## New Files

### `src/curl/curl.h`
Minimal libcurl API header declaring only the types, enums, and function
signatures actually used by DigiAsset Core. Allows the codebase to `#include
<curl/curl.h>` without installing libcurl.

### `src/curl_stubs.cpp`
Full WinHTTP-backed implementation of the libcurl API subset:

- **Persistent connections** — `CurlHandle` stores `HINTERNET hSession` and
  `HINTERNET hConnect` and reuses them across `curl_easy_perform()` calls on
  the same `CURL*` handle. Eliminates a TCP+HTTP handshake on every RPC call to
  DigiByte Core, reducing per-call overhead significantly.
- **Reconnect-on-stale** — if a keep-alive connection goes stale the request
  is retried once with a fresh connection handle before returning an error.
- **Auth header injection** — user:password credentials embedded in the URL
  are Base64-encoded and sent as an `Authorization: Basic` header.
- **Correct error mapping** — `ERROR_WINHTTP_CANNOT_CONNECT` and
  `ERROR_WINHTTP_CONNECTION_ERROR` map to `CURLE_COULDNT_CONNECT`, whose
  `curl_easy_strerror()` string (`"Could not connect to server"`) matches the
  substring checked by `IPFS::_command()` so that a non-running local IPFS
  daemon is silently ignored rather than logged as CRITICAL errors.

### `src/openssl/bio.h` and `src/openssl/evp.h`
Stub OpenSSL headers providing the minimum type definitions needed to compile
the jsonrpccpp HTTP connector without installing OpenSSL.

### `src/openssl_stubs.cpp`
No-op implementations of the OpenSSL BIO and EVP functions referenced by
jsonrpccpp's HTTP connector. The connector's SSL code path is not exercised
(all connections are plain HTTP to localhost/LAN endpoints), so stubs suffice.

### `src/sqlite3.h` and `src/sqlite3.c`
Real **SQLite 3.47.2 amalgamation**. Replaces the previous stub that returned
`SQLITE_DONE` from every statement, causing `Database::getBlockHeight()` to
throw "Database Exception: Select failed" on startup. Compiled directly as part
of the project — no separate library or DLL needed.

### `src/jsonrpccpp/`
Vendored source copy of the libjsonrpccpp `common`, `client`, and `server`
modules, with a custom `src/jsonrpccpp/common/jsonparser.h` that bridges to the
locally-installed jsoncpp headers. Eliminates the system package dependency.

---

## Modified Files

### `CMakeLists.txt` and `src/CMakeLists.txt`
- On MSVC: select the stub/vendored sources instead of system packages
- Link `winhttp.lib`
- Add Windows-specific preprocessor definitions
- Compile `src/sqlite3.c` as a direct source unit

### `src/Database.h` and `src/Database.cpp`

**Transaction nesting guard** — Added `int _transactionDepth = 0` member.
`startTransaction()` increments the counter and only issues `BEGIN TRANSACTION`
when depth goes from 0 → 1. `endTransaction()` decrements and only issues
`END TRANSACTION` when depth returns to 0. This prevents a nested call (e.g.
from block-level batching inside a pre-existing transaction) from prematurely
committing an outer transaction.

**Write verification bypass** — When `verifydatabasewrite=0` is set in
`config.cfg`, the database now executes:
```
PRAGMA synchronous = OFF
PRAGMA journal_mode = MEMORY
```
This eliminates the `fsync()` / file-flush overhead on every SQLite write,
which was the dominant bottleneck during initial blockchain sync. Estimated sync
time dropped from ~4 days to approximately 26 hours.

### `src/ChainAnalyzer.cpp`

**Block-level transaction batching** — All database writes for a single block
are wrapped in one `startTransaction()` / `endTransaction()` pair inside
`phaseSync()`. For dense blocks with many transactions this reduces the number
of SQLite `BEGIN`/`COMMIT` round-trips from O(txcount) to 1.

**Cleanup** — Removed temporary debug `cerr` statements; restored
`startupFunction()` to its original exception-propagation pattern.

### `src/RPC/Server.h` and `src/RPC/Server.cpp`

Removed the `ctorInitTrace()` static-initializer scaffolding that was added
during early debugging (it served no runtime purpose). Removed all temporary
`std::cerr << "DEBUG:"` statements. The constructor now logs a single
`"RPC Server listening on port N"` message through the normal `Log` subsystem.

### `src/main.cpp`

- Log level for file output restored to config-driven value
  (`config.getInteger("logfile", Log::WARNING)`)
- RPC server launched in a detached thread that calls `server->start()`
  (previously had a no-op lambda during debugging)
- Chain Analyzer start wrapped in try/catch that logs via `Log::CRITICAL`
- All temporary debug output removed

### `src/CurlHandler.cpp`, `src/Threaded.cpp`, `src/crypto/SHA256.cpp`, `src/utils.cpp`, `src/utils.h`, `src/RPC/Cache.h`

Miscellaneous Windows/MSVC compatibility fixes:
- Missing `#include` directives for standard headers
- `int`/`size_t` signed-unsigned comparison warnings treated as errors under MSVC
- Platform-specific preprocessor guards (`#ifdef _WIN32`)
- `constexpr` and `inline` specifier adjustments for MSVC conformance

---

## CLI, Web, and Test Targets

### `cli/CMakeLists.txt`
Rewrote for Windows: uses WinHTTP curl stubs and local jsonrpccpp sources
on MSVC instead of `find_package(CURL)`. Builds `digiasset_core-cli.exe`.

### `web/CMakeLists.txt`
Added Boost 1.82.0 NuGet include path so Boost Beast headers are found.
Added `_WIN32_WINNT` definition. Builds `digiasset_core-web.exe`.

### `tests/CMakeLists.txt`
- Uses C++20 on MSVC (required for designated initializers in test code)
- Added `/Zc:char8_t-` flag to preserve `u8""` as `const char[]`
- Links Windows-specific libraries (winhttp, jsoncpp_static, jsonrpccpp)

### `tests/Base58Tests.cpp`
Changed `std::uniform_int_distribution<uint8_t>` to `unsigned int` —
MSVC's STL does not allow `uint8_t` as a distribution type.

### `src/DigiAssetRules.h` and `src/DigiAssetRules.cpp`
Made `operator==` `const` to fix C++20 ambiguity with gtest's
`EXPECT_TRUE` macro (C++20 synthesizes reverse `operator==` candidates).

---

## Configuration

Add the following line to `config.cfg` to enable the write-performance mode
(recommended during initial sync; safe to leave on for normal operation if you
do not need crash-safe durability):

```
verifydatabasewrite=0
```

---

## Build Requirements (Windows)

- Visual Studio 2022 (Community or higher), C++ desktop workload
- CMake 3.20+
- Boost (installed via the project's NuGet restore, or manually to `src/boost/`)
- jsoncpp (header-only path expected at `src/jsoncpp/` or system include)
- No other external libraries required — curl, OpenSSL, SQLite3, and
  jsonrpccpp are all provided by vendored sources in this repository
