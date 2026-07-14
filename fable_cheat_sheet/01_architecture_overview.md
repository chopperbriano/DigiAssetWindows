# Architecture Overview

High-level map of the repo so the other cheat-sheet files make sense in context.
Verified directly against the tree on 2026-07-13, on `last_tasks` branch.

## Top-level layout

```
src/     — digiasset_core daemon (the core logic + RPC server)
cli/     — digiasset_core-cli (generic JSON-RPC passthrough client)
qt/      — Qt GUI app (wraps the daemon, currently one tab: sync progress)
web/     — digiasset_core-web, a docs HTTP server (port 8090)
tests/   — Google Test suite (Google_Tests_run target)
```

Top-level `CMakeLists.txt` has 4 build options, all default ON except tests:
```
option(BUILD_CLI "Build CLI" ON)
option(BUILD_WEB "Build Web Server" ON)
option(BUILD_QT "Build QT" ON)
option(BUILD_TEST "Build Tests" OFF)
```
Each subdirectory is pulled in with `ADD_SUBDIRECTORY(...)` guarded by its option.
See `02_rpc_cli_and_build.md` for exact build commands/targets.

## `src/` — the daemon core

Flat-ish structure, no deep subfolders except `RPC/`, `PermanentStoragePool/`, and
`crypto/`. Key files (71 RPC method files live under `src/RPC/Methods/` — see
`02_rpc_cli_and_build.md` for the full list):

- `main.cpp` — entry point.
- `AppMain.h/.cpp` — process-wide singleton holding pointers to the major
  subsystems (DigiByte Core RPC client, IPFS client, Database, PermanentStoragePool,
  ChainAnalyzer, RPC cache). Tests construct/tear down an `AppMain` to get a working
  environment — see `05_tests_and_psp.md`.
- `DigiByteCore.h/.cpp` (+ `DigiByteCore_Types.h`, `DigiByteCore_Exception.h`) —
  the client that talks to the actual DigiByte Core wallet/node over its own
  JSON-RPC. This is where `fundrawtransaction`/`signrawtransactionwithwallet`/
  `sendrawtransaction`/wallet calls would be issued from. Full detail in
  `03_transaction_encoding_and_database.md`.
- `DigiByteTransaction.h/.cpp` — parses/builds individual transactions in terms of
  DigiAsset semantics (encode/decode asset issuance & transfer data). This is the
  file with the `addDigiByteOutput` stub and missing `addDigiAssetOutput` — the
  crux of tasks 1 & 2. Full detail in `03_transaction_encoding_and_database.md`.
- `DigiAsset.h/.cpp`, `DigiAssetTypes.h/.cpp`, `DigiAssetRules.h/.cpp`,
  `DigiAssetConstants.h` — the asset data model itself (what an "asset" is: id,
  rules, decimals, etc.) as opposed to the transaction encoding.
- `Database.h/.cpp` (+ `Database_Statement.*`, `Database_LockedStatement.*`) —
  the sqlite3 wrapper and all cached prepared statements. See
  `DATABASE_OPTIMIZATION_PLAN.md` (repo root) for a schema-level summary (15 core
  tables, schema version 6). Asset/balance-relevant queries detailed in
  `03_transaction_encoding_and_database.md`.
- `ChainAnalyzer.h/.cpp` — walks the chain, feeds transactions through
  `DigiByteTransaction` decode logic, updates the Database. The "indexer" loop.
- `IPFS.h/.cpp` — local IPFS node client (pin/fetch asset metadata by CID).
- `PermanentStoragePool/` — a subsystem for a storage-proof/payment scheme
  (background context + bugs in `05_tests_and_psp.md`).
- `KYC.h/.cpp`, `DigiByteDomain.h/.cpp` — KYC data and DigiByte "digibyte domain"
  (human-readable name → address) resolution, both referenced by RPC methods.
- `RPC/` — the JSON-RPC server. See `02_rpc_cli_and_build.md` for full detail:
  `Server.cpp` (routing/dispatch), `MethodList.cpp` (auto-generated registry),
  `Methods/*.cpp` (one file per method, 71 total), `Cache.cpp` (RPC response cache),
  `Response.h` (response envelope helper).

## `cli/` — digiasset_core-cli

A thin generic JSON-RPC client: takes a method name + params from argv and posts
them straight to the daemon's RPC server. No hardcoded method list, so **any new
RPC method automatically becomes available via the CLI** — "add CLI support" for
tasks 1/2/3 reduces to "add the RPC method". Confirmed with source quote in
`02_rpc_cli_and_build.md`.

## `qt/` — GUI

Launches the daemon in-process, shows a splash/sync-progress screen, then a
`QTabWidget` that currently has exactly one tab (`qt/tabs/SyncTab`). New GUI
work (create-asset tab, send-asset tab, balances tab) should follow the same
tab pattern and talk to the daemon via `RPCLoader`. Full detail, including where
in `main.cpp` new tabs get wired in, in `04_qt_gui_and_web_docs.md`.

## `web/` — documentation server

Not the DigiAsset functionality itself — a small HTTP server (port 8090) that
serves `web/index.html` plus RPC method docs: `web/rpc/*.html` (DigiByte wallet's
own RPC docs) and `src/RPC/Methods/*.html` (this project's custom method docs,
one per `.cpp` file, same directory). Task 5 documentation work lives here and in
repo-root `readme.md`. Full detail in `04_qt_gui_and_web_docs.md`.

## `tests/`

Google Test suite. Build target `Google_Tests_run`. Uses a partial-chain sqlite
fixture under `tests/testFiles/` for tests that don't need a live/synced chain.
Full file-by-file map, coverage gaps, and the PermanentStoragePool bug context in
`05_tests_and_psp.md`.

## Two build directories exist in the repo root

`build/` and `cmake-build-release/` both exist at repo root. `build_and_test.sh`
(a Linux-focused script) uses `build/`. Check `02_rpc_cli_and_build.md` for which
one is the actively-used one on this Mac (CLion tends to manage
`cmake-build-release/` automatically) before assuming either is current/clean —
prefer reconfiguring fresh if unsure rather than trusting stale cache.
