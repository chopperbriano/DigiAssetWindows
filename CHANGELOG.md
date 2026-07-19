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

## 0.3.3-win.102 (current) — accurate sync auto-recovery diagnostics

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
