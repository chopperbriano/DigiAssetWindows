# Building DigiAsset for Windows

This page is for developers. To run a node you do not need to build anything; use
the one-line installer in the [readme](readme.md#quick-install-recommended).

Manual setup is not supported for operators; use the installer. Developers: see
[example.cfg](example.cfg) for every key.

## Repository layout

| Path | What |
|---|---|
| `setup-digiasset.ps1` | **Node installer — the one-liner.** |
| `install-digibyte.ps1` | Standalone **DigiByte Core only**: install + configure + seed. No IPFS/node/pool. |
| `node/` | Node operator helpers: `monitor-node.ps1`, `stop-node.ps1`, `update-binaries.ps1`, `memwatch.ps1`. |
| `snapshots/` | Fast-sync tooling: `make-snapshot.ps1` / `publish-snapshot.ps1` (create), `seed-digibyte.ps1` (consume). |
| `pool/`, `setup-pool.ps1` | Pool-server code + installer. Pool operators only. |
| `src/`, `cli/` | The node/analyzer C++ and command-line client. See **[ARCHITECTURE.md](ARCHITECTURE.md)**. |
| `tests/` | `Unit_Tests_run` (release gate) and `Google_Tests_run` (needs a live node). |
| `web/` | The node's web console (shipped as `web.zip`). |
| `example.cfg` | Fully-commented `config.cfg` reference. |

Paths on an installed machine: DigiByte in `C:\DigiByte` (blockchain in
`C:\DigiByte\Data`, `digibyte.conf` in `C:\DigiByte`); the node + pool in
`C:\DigiAssetWindows`.

## Build on Windows

This fork builds a Windows version with Visual Studio and MSVC in the main branch, with upstream tracking in the `upstream-master` branch. Upstream changes from [DigiAsset-Core/DigiAsset_Core](https://github.com/DigiAsset-Core/DigiAsset_Core) are merged periodically.

Most dependencies (libcurl, OpenSSL, SQLite3, libjsonrpccpp) are replaced by vendored source files or Windows-native stubs (WinHTTP), so no vcpkg or external package manager is needed beyond the jsoncpp and libjson-rpc-cpp subprojects that are already in the repo.

### Prerequisites

- **Visual Studio 2022 or later** (Community or higher) with the "Desktop development with C++" workload
- **CMake 3.20+** (included with VS — select "C++ CMake tools for Windows" in the installer)

### Clone the Repository

```cmd
git clone --recursive https://github.com/chopperbriano/DigiAssetWindows.git
cd DigiAssetWindows
```

The `--recursive` flag is required to fetch the jsoncpp and libjson-rpc-cpp submodules. If you already cloned without it, run:

```cmd
git submodule update --init --recursive
```

### Build JsonCpp Library

```cmd
.\config-jsoncpp.bat
```

Open `jsoncpp\build\jsoncpp.sln` in Visual Studio. Select your build configuration (Debug or Release). Build `ALL_BUILD`, then build `INSTALL`.

### Build LibJson-RPC Library

```cmd
.\config-libjson-rpc.bat
```

Open `libjson-rpc-cpp\build\libjson-rpc-cpp.sln`. Use the **same** configuration as above. Build `ALL_BUILD`, then `INSTALL`.

> The pinned upstream commit calls `cmake_policy(SET CMP0042 OLD)`, which CMake 4.x
> refuses outright, so this dependency cannot configure as-shipped. `config-libjson-rpc.bat`
> applies `patches\libjson-rpc-cpp-cmp0042.patch` for you before running CMake — it is
> idempotent, so re-running the script is safe. This leaves the submodule with a modified
> `CMakeLists.txt`, which is expected; `.gitmodules` marks both dependency submodules
> `ignore = dirty` so that build-time churn does not show up in `git status`.

### Install Boost (required for web server)

```cmd
nuget.exe install boost -Version 1.82.0 -OutputDirectory packages
```

If you don't have `nuget.exe`, download it from https://www.nuget.org/downloads

### Build DigiAsset for Windows

```cmd
.\config.bat
```

Open `build\digiasset_core.sln` in Visual Studio, select the **same** configuration (Debug or Release) as the libraries above, and build `ALL_BUILD`.

Or build from a Developer Command Prompt:

```cmd
cd build
msbuild src\DigiAssetWindows.vcxproj /p:Configuration=Release
```

The `DigiAssetWindows.exe` binary will be in `build\src\Release\` (or `Debug\`). This single executable includes the core sync engine, RPC server, and web UI server.

## Optional Build Targets

CMake options:

```cmd
cmake -B build -S . -DBUILD_TEST=ON
```

| Option | Default | Binary | Description |
|---|---|---|---|
| `BUILD_CLI` | ON | `DigiAssetWindows-cli.exe` | Command-line RPC client |
| `BUILD_WEB` | OFF | `digiasset_core-web.exe` | Upstream's standalone web server (legacy; the console is built into the main exe) |
| `BUILD_TEST` | OFF | `Unit_Tests_run.exe`, `Google_Tests_run.exe` | Test suites (see below) |

`BUILD_TEST=ON` builds two suites:

- **`Unit_Tests_run`** — needs nothing running. This is the release gate; every
  test must pass. Run it with `run_tests.bat` or
  `build\tests\Release\Unit_Tests_run.exe`.
- **`Google_Tests_run`** — needs a live DigiByte Core and IPFS. Not part of the
  release gate.

`DigiAssetPoolServer.exe` (the optional pool server) is **built automatically on
Windows/MSVC** — no flag needed. It lands in `build\pool\Release\`. You only run it
if you operate your own pool; see [POOL-SETUP.md](POOL-SETUP.md).

Releasing is covered in [docs/releasing.md](docs/releasing.md).

## Performance Tuning

For faster initial blockchain sync, add to `config.cfg`:

```
verifydatabasewrite=0
```

This disables SQLite write verification (fsync), significantly reducing sync time.

## Snapshots and chain.db

New installs restore a pre-synced snapshot (the DigiByte blockchain and the node's
`chain.db`) instead of syncing from genesis. Snapshots are produced by
`snapshots/make-snapshot.ps1`. It stops the node **cleanly** (and aborts rather than
snapshot a node that will not stop, since a hard kill can leave `chain.db` torn),
then archives `chain.db` together with its `-wal` and `-shm` sidecars. Shipping the
sidecars is what makes the copy consistent; do not copy `chain.db` alone out from
under a running daemon. Details: [snapshots/README.md](snapshots/README.md).

## Before you touch the transfer path

Read [docs/asset-rules-and-burns.md](docs/asset-rules-and-burns.md). A rule
violation burns every asset in the transaction, the sender's change included.

## Contributing

- If submitting pull requests please utilize the `.clang-format` file to keep things standardized.
- Upstream changes are tracked on the `upstream-master` branch and merged into `master` periodically.
