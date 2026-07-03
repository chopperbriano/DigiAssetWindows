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
