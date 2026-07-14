# RPC Layer, CLI, and Build System

## 1. All existing RPC methods (`src/RPC/Methods/*.cpp`)

Each file is `namespace RPC { namespace Methods { extern const Response <name>(const Json::Value& params) {...} } }`.
Docstrings in these files are unusually complete — always read the file's top comment block first, it's the spec.

| Method | File | What it does |
|---|---|---|
| `version` | `version.cpp` | Returns daemon version string. Simplest possible method — good template. |
| `syncstate` | `syncstate.cpp` | `{count, sync}` — DGB height + DigiAsset sync state (0=synced,1=stopped,2=init,3=rewind,4=optimizing). |
| `getaddressholdings` | `getaddressholdings.cpp` (43 lines) | `{assetIndex: qty}` for ONE address, from `db->getAddressHoldings(address)`. assetIndex 1 = DigiByte itself if `storenonassetutxo=1`. **No wallet-wide aggregation, no asset metadata** — task 3 (balance) needs this built out. |
| `getaddresskyc` | `getaddresskyc.cpp` | `{address, country?, name?|hash?}` from `db->getAddressKYC(address)`. |
| `getassetdata` | `getassetdata.cpp` (89 lines) | Two calling conventions: by `assetIndex` (int) or by `assetId`+optional `txid`/`vout` (string form, needed for multi-index assets). Returns `DigiAsset::toJSON()`. Also sets total count and PSP membership on the asset before returning. Good reference for how `Database::getAsset`/`getAssetIndex` work. |
| `getassetholders` | `getassetholders.cpp` | `{address: qty}` for ALL holders of one asset (by index or id). |
| `getassetindexes` | `getassetindexes.cpp` | List of assetIndexes sharing one assetId (multi-index assets). |
| `getdgbequivalent` | `getdgbequivalent.cpp` | Converts an exchange-rate amount to DGB sats for one address/index. Throws if sync >120 blocks behind. |
| `getdomainaddress` | `getdomainaddress.cpp` | Resolves a DigiByte domain name → address (`DigiByteDomain::getAddress`). |
| `getexchangerates` | `getexchangerates.cpp` (72 lines) | List of exchange rates, optionally at a historical height. See `DigiAssetConstants::standardExchangeRates` in `src/DigiAsset.cpp` for the standard rate addresses (e.g. USD rate lives at `dgb1qunxh378eltj2jrwza5sj9grvu5xud43vqvudwh` index 1). |
| `getpsp` | `getpsp.cpp` | Data about a Permanent Storage Pool by index. |
| `listaddresshistory` | `listaddresshistory.cpp` | txids an address was involved in (bounded by pruning level). |
| `listassetissuances` | `listassetissuances.cpp` | All asset issuances, basic format. |
| `listassets` | `listassets.cpp` (117 lines) | Paged list of assets by issuance height, with a `filter` object supporting PSP filtering. `basic` mode returns `{assetIndex, assetId, cid, height}`; full mode returns `DigiAsset::toJSON()`. |
| `listlastassets` | `listlastassets.cpp` (129 lines) | Same idea as `listassets` but paged from the newest end (`firstAsset` param). |
| `listlastassetspageindexes` | `listlastassetspageindexes.cpp` | Returns `{start, skips[]}` pagination markers for `listlastassets` — use when building "load more" UI. |
| `listlastblocks` | `listlastblocks.cpp` | Recently processed block list. |
| `listunspent` | `listunspent.cpp` (221 lines, the biggest method) | UTXOs for wallet or given addresses, DGB-style params plus DigiAsset extensions: `includeAsset` (filter to one asset / funds-only / all), `detailedAssetData`. **This is the closest existing thing to an asset-aware balance/UTXO view — study this closely for tasks 2 and 3.** |
| `gettxout` | `gettxout.cpp` | Same as DigiByte's `gettxout` but with `DigiAsset::toJSON` fields merged in. |
| `getrawtransaction` | `getrawtransaction.cpp` | Same idea, DGB raw tx + asset fields. **Currently has uncommitted local changes** (+37/-10, see `LAST_TASKS_NOTES.md`) — check `git diff` before editing. |
| `send` | `send.cpp` | Passthrough to DGB wallet `send`, domains resolved to addresses first. **DGB-only, not asset-aware** (confirmed — this is why task 2 needs new code, not a tweak here). |
| `sendmany` | `sendmany.cpp` | Same domain-resolution passthrough pattern, for `sendmany`. |
| `sendtoaddress` | `sendtoaddress.cpp` (40 lines) | Same pattern, simplest of the three — good template for the domain-resolution idiom: `DigiByteDomain::isDomain(...)` → `DigiByteDomain::getAddress(...)`, then forward via `AppMain::GetInstance()->getDigiByteCore()->sendcommand(...)`. |
| `addressstats` | `addressstats.cpp` | Time-bucketed address stats (new/used/balance tiers/quantum-insecure counts). |
| `algostats` | `algostats.cpp` | Time-bucketed mining/difficulty stats per algo. |
| `resyncmetadata` | `resyncmetadata.cpp` | Re-pins all IPFS metadata (fire-and-forget). |
| `getencryptedkey` | `getencryptedkey.cpp` | Encrypted data blob for address(es). |
| `getipfscount` | `getipfscount.cpp` | IPFS job queue depth. |
| `getoldstreamkey` | `getoldstreamkey.cpp` | Legacy DigiAsset Stream compat shim — marked for eventual removal, don't build on it. |
| `getrandom` | `getrandom.cpp` | Provably-fair random number derived from a txid/vout or block height. Needs unpruned data. |
| `shutdown` | `shutdown.cpp` | Stops the daemon. |
| `debugwaittimes` | `debugwaittimes.cpp` | Perf/lock-wait diagnostics string. |
| `asyncstart` / `asyncget` / `asyncclear` | `async*.cpp` | Generic "fire a slow RPC method in background, poll for result" wrapper — takes a method name + its params as `params[0..]`, results cached indefinitely with a `cacheTime` field. Useful if a new create/send-asset method turns out to be slow. |

**31 methods total.** None of them build or sign asset issuance/transfer transactions — that logic doesn't exist yet anywhere in `src/RPC/Methods/`, confirming `LAST_TASKS_NOTES.md`'s finding.

## 2. How a new RPC method gets registered — exact mechanism

`src/RPC/MethodList.h` and `src/RPC/MethodList.cpp` are marked `///This file is automatically generated by CMakeLists.txt do not edit` — and they mean it literally. The generation logic is in `src/CMakeLists.txt:10-49`:

```cmake
file(GLOB RPC_METHOD_SOURCES RPC/Methods/*.cpp)   # src/CMakeLists.txt:11

foreach(SOURCE_FILE ${RPC_METHOD_SOURCES})
    get_filename_component(METHOD_NAME ${SOURCE_FILE} NAME_WE)   # filename minus .cpp = method/function name
    ...writes an `extern const Response <METHOD_NAME>(...)` decl into MethodList.h
    ...writes a `{"<METHOD_NAME>", Methods::<METHOD_NAME>}` map entry into MethodList.cpp
endforeach()
```

**To add a new RPC method:**
1. Create `src/RPC/Methods/<newmethodname>.cpp` — the filename (without `.cpp`) IS the RPC method name AND the C++ function name. No other place to register it.
2. Implement exactly this skeleton (copy `version.cpp` as the minimal template, or `sendtoaddress.cpp` for one that forwards to the wallet):
   ```cpp
   #include "AppMain.h"
   #include "RPC/Response.h"
   #include "RPC/Server.h"
   #include <jsoncpp/json/value.h>

   namespace RPC {
       namespace Methods {
           /** doc comment describing params[] and return shape — this IS the API docs source */
           extern const Response <newmethodname>(const Json::Value& params) {
               // validate params, do work, build Response
           }
       }
   }
   ```
3. **Critical gotcha:** `file(GLOB ...)` runs at CMake *configure* time, not build time. Adding the `.cpp` file alone and running `make`/`cmake --build` will NOT pick it up — you must re-run `cmake ..` (or touch `CMakeLists.txt`) to force reconfiguration before building, otherwise the new method silently doesn't exist in `MethodList.cpp`.
4. `Response` (see `src/RPC/Response.h`/`.cpp`) supports `setResult()`, `setBlocksGoodFor(n)` (RPC cache TTL in blocks; `-1` disables caching — used by all `send*` methods since results must never be cached), and `addInvalidateOnAddressChange(address)` (invalidates cache when that address's state changes — used by `getaddressholdings`/`getaddresskyc`).
5. Also make a matching `.html` doc file for the method (see section on web docs / task 5) — `TODO_TESTS.md`/`LAST_TASKS_NOTES.md` both flag that these must stay in sync.

## 3. Request routing — `src/RPC/Server.cpp`

- `Server::executeCall()` (line 282) is the dispatcher:
  1. Checks `isRPCAllowed(methodName)` against the `rpcallow` config map (safe-mode allowlist).
  2. Checks the RPC cache (`AppMain::GetInstance()->getRpcCache()`).
  3. **If `methods.find(methodName) != methods.end()`** (i.e. it's one of the 31 custom methods in the CMake-generated map) → calls it directly.
  4. **Else** → falls back to `app->getDigiByteCore()->sendcommand(methodName, params)`, i.e. forwards verbatim to the actual DigiByte Core wallet RPC. This is how e.g. `getblockchaininfo`, `getbalance`, `listtransactions` etc. "just work" without being reimplemented here.
  5. A hardcoded `cacheableRpcCommands` vector (Server.cpp:31-56) lists which *forwarded* (non-custom) methods are safe to cache (mostly read-only chain-state calls); anything forwarded that's not in that list gets `setBlocksGoodFor(-1)`.
- HTTP parsing, basic auth, and JSON-RPC envelope handling are all hand-rolled in this file (no external JSON-RPC server library used for `digiasset_core` itself, despite `jsonrpccpp/client` being used for the *outgoing* call to DigiByte Core).
- Listens on `rpcassetport` config value, default port **14024** (note: this is DIFFERENT from DigiByte Core's own RPC port 14022 seen in `~/.digibyte/digibyte.conf`).

## 4. `cli/` — generic JSON-RPC passthrough, confirmed

`cli/main.cpp` (88 lines) is the entire CLI. Key excerpt (lines 10-61):

```cpp
int main(int argc, char* argv[]) {
    string command = argv[1];                    // any string — no hardcoded method list
    Json::Value args = Json::arrayValue;
    // ...argv[2..] parsed into a JSON array (auto-detects int/double/bool/JSON-array-or-object/string)...
    DigiByteCore dgb;
    dgb.setFileName("config.cfg", true);
    dgb.makeConnection();
    cout << dgb.sendcommand(command, args) << "\n";
}
```

There is **no switch/if-chain over method names anywhere in the CLI.** It connects to `digiasset_core`'s own RPC port (via `config.cfg`, same file the daemon reads) and forwards whatever method name + args you typed. This confirms `LAST_TASKS_NOTES.md`: **"CLI support" for any new feature == "an RPC method exists for it."** Once a new `createasset`/`sendasset`/`getbalance`-style method is added per section 2 above, it is automatically usable from `digiasset_core-cli <methodname> <args...>` with zero CLI changes.

Note: the CLI talks through the SAME dispatcher in `Server.cpp` — so CLI calls to unknown methods also transparently forward to the DigiByte wallet.

## 5. Build system

Top-level `CMakeLists.txt` (repo root) options:
```cmake
option(BUILD_CLI "Build CLI" ON)
option(BUILD_WEB "Build Web Server" ON)
option(BUILD_QT "Build QT" ON)
option(BUILD_TEST "Build Tests" OFF)   # must pass -DBUILD_TEST=ON explicitly
```

Subdirectories/targets:
| Dir | CMake project | Target name | Notes |
|---|---|---|---|
| `src/` | `digiasset_core` | `digiasset_core` (executable) | Always built. Contains the MethodList generation logic. Also builds `libdigiasset_core_lib.a` (used by tests). |
| `cli/` | `digiasset_core-cli` | `digiasset_core-cli` | Gated by `BUILD_CLI`. |
| `qt/` | `digiasset_core-qt` | `digiasset_core-qt` | Gated by `BUILD_QT`. |
| `web/` | `digiasset_core-web` | `digiasset_core-web` | Gated by `BUILD_WEB`. Doc server, port 8090. |
| `tests/` | `Google_tests` | `Google_Tests_run` | Gated by `BUILD_TEST`; pulls in googletest submodule (version pinned `GOOGLETEST_VERSION 1.11.0`). |

All binaries land in **`<repo-root>/bin/`** regardless of which build directory you configure from (`CMAKE_RUNTIME_OUTPUT_DIRECTORY` is hardcoded to `${CMAKE_SOURCE_DIR}/bin`, top-level `CMakeLists.txt` lines 22-24) — so `bin/` is shared/clobbered by whichever build directory you build from last.

**Two build directories exist in the repo root — pick the live one carefully:**
- `build/` — `CMakeCache.txt` dated **2026-04-28**, `BUILD_TEST:BOOL=ON`. This is the more recently used one; its build timestamp matches the binaries currently sitting in `bin/` (`digiasset_core`, `Google_Tests_run` both dated Apr 28). **Treat this as the active build dir.**
- `cmake-build-release/` — `CMakeCache.txt` dated **2026-04-18**, has a `cli/` subfolder already configured, `BUILD_TESTS:UNINITIALIZED=ON` (note: this cache var name is `BUILD_TESTS` not `BUILD_TEST`, suggesting it may be a stale/CLion-autogenerated config that predates a variable rename). Likely CLion's default build-dir name. **`TODO_TESTS.md` tells you to build here — that instruction may be stale; prefer `build/` unless you confirm `cmake-build-release` was intentionally kept in sync.**

Recommended verification before trusting either: `date -r build/CMakeCache.txt` vs `date -r cmake-build-release/CMakeCache.txt`, and re-run `cmake` configure in whichever you use (remember: required anyway after adding new RPC method files, see section 2).

`build_and_test.sh` (repo root) is a **Linux-only** (`apt-get`) script that does a from-scratch `rm -rf build && cmake .. -DBUILD_TEST=ON -DBUILD_CLI=ON -DBUILD_WEB=ON` + build + run tests. Not directly usable on this macOS dev machine as-is (relies on `apt-get`, `nproc`), but its CMake invocation flags are the reference for what a full Linux CI build should pass.
