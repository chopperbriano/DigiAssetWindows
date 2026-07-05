# DigiAsset for Windows — Architecture

A Windows-native port of DigiAsset Core (credit: upstream **DigiAsset Core** by
mctrivia). It packages a full DigiAsset node — and an optional Permanent Storage
Pool server — into one-command Windows installers, GUI-first dashboards, and
fast-sync snapshots so an ordinary Windows PC can host DigiAsset content for the
DigiByte network.

This document is the map. It reflects the code as it actually is today, including
the historical dead-ends that are called out inline so you don't chase them.

---

## 1. Vision

- **One command, whole stack.** A single PowerShell script (`setup-digiasset.ps1`,
  runnable via a copy-paste `iwr` one-liner) installs and wires up all three
  moving parts on a fresh machine: **DigiByte Core** (the wallet/blockchain),
  **IPFS / Kubo** (content storage), and the **DigiAsset for Windows node** (the
  indexer + dashboard). It writes every config file, opens the firewall, and
  registers boot tasks.
- **Fast-synced from hosted snapshots.** A fresh install normally does *not* wait
  the ~week it takes to sync DigiByte from genesis. Instead it downloads,
  SHA256-verifies, and extracts a pre-synced blockchain + `chain.db` from a
  hosted **Cloudflare R2** snapshot (`snapshot.json` manifest). A secondary
  bootstrap path pulls `chain.db` over IPFS from a pinned CID.
- **Self-healing.** The same script re-runs on every boot in `-Mode Service`
  (as SYSTEM): it checks GitHub/IPFS for newer component versions, updates them
  with verified downloads and graceful restarts, health-checks the whole stack,
  and aggressively repairs it (restart tasks, re-download corrupt files,
  re-open firewall) — only alerting the user if healing fails.
- **GUI-first.** The node and the pool server each take over the console with an
  in-place VT100 **dashboard** (`ConsoleDashboard`, `PoolDashboard`) rather than
  scrolling logs. The installer also auto-starts the GUI wallet, IPFS Desktop,
  and the node dashboard on logon.

---

## 2. Module map & data flow

The core data pipeline, left to right:

```
DigiByte Core  →  ChainAnalyzer  →  chain.db (SQLite)  →  asset/UTXO model
   (RPC)            (indexer)         (Database)            (DigiAsset*)
                                          │
                                          ▼
                                    IPFS pinning  →  Permanent Storage Pools
                                     (Kubo API)         (PSP list)
                                          │
                                          ▼
                          RPC server · Web server · CLI  (query surfaces)
```

### Entry points & wiring

- **`src/main.cpp`** — the node exe (`DigiAssetWindows.exe`) entry point. On
  first launch it runs an interactive **config wizard** that writes `config.cfg`.
  Then, in dependency order, it: optionally bootstraps `chain.db` from IPFS
  (with retry/fallback to sync-from-scratch), connects to DigiByte Core (retry
  loop, waits for the wallet to reach bootstrap height), opens the Database,
  starts the IPFS handler, the Permanent Storage Pool list, the RPC cache, the
  Chain Analyzer, the RPC server (own thread), and the Web server — then idles
  until Ctrl+C / SIGTERM / dashboard quit key and tears down.
- **`src/AppMain.*`** — thread-safe **singleton service locator**. Holds (does
  not own) pointers to the long-lived subsystems (Database, IPFS, DigiByteCore,
  PSP list, ChainAnalyzer, RPC cache/server, WebServer) so any code can reach
  them without threading them through call chains. `main()` constructs the
  objects and registers them via `set*()`.

### The pipeline

- **`src/DigiByteCore.*`** — typed C++ facade over a DigiByte Core full node's
  JSON-RPC API (the only way the node talks to `digibyted`). Serializes calls
  under a mutex, one method per RPC, returns the structs in
  `DigiByteCore_Types.h`. `getBlockVerbose()` pre-loads a whole block's
  transactions into an in-memory TX cache so block processing needs no extra
  round-trips. Translates raw RPC faults into friendly exception types
  (`exceptionCoreOffline`, `exceptionDigiByteCoreNotConnected`).
- **`src/ChainAnalyzer.*`** — the **blockchain indexing engine** and heart of the
  node. Runs on its own worker thread (`Threaded`). Walks the chain block-by-block
  from Core, hands each transaction to `DigiByteTransaction` for DigiAsset
  parsing, and persists results to the Database. Handles **fork rewinds**
  (`phaseRewind`), the catch-up/tip-follow **sync loop** (`phaseSync`), and
  periodic **pruning** (`phasePrune`) to keep the DB small. Sync state
  (`getSync()` / `getSyncHeight()`) is what the dashboard and RPC report.
- **`src/Database.*`** (+ `Database_Statement.*`, `Database_LockedStatement.*`) —
  the single **SQLite-backed store** (`chain.db`) of all chain-derived state:
  assets, UTXOs, blocks, exchange rates, KYC, votes, DigiByte-Domain mappings,
  the IPFS job queue, Permanent Storage Pool membership (`permanent` / pool
  tables), unknown OP_RETURNs, encrypted keys, and periodic algo/address stats.
  Every query is a pre-compiled `Statement` accessed through a `LockedStatement`
  for thread safety. In-RAM caches (flags, watch addresses, a two-generation
  non-asset-UTXO cache) avoid DB/RPC round-trips. **This one class underpins both
  deployables** — the node uses it as its chain index; the pool concept reuses
  the permanent/PSP tables.
- **`src/DigiAsset*.*`** — the asset domain model:
  - `DigiAsset.*` — one DigiAsset (or a held quantity): decode from an issuance
    OP_RETURN, rebuild from a DB row, serialize to JSON. Carries identity
    (assetId, metadata CID, issuer KYC) and rules.
  - `DigiAssetRules.*` — transfer/issuance rule enforcement over a tx's
    inputs/outputs (voting, expiry, royalties, KYC, exchange-rate pegs, etc.).
  - `DigiAssetTypes.*` — shared value types (`AssetUTXO`, `AssetHolder`,
    `ExchangeRate`, `AssetBasics`, …).
  - `DigiByteTransaction.*` — parses a raw DigiByte tx into its DigiAsset effects
    (issuance/transfer/burn, metadata, rules) — the bridge ChainAnalyzer uses.
  - `DigiByteDomain.*` — the DigiByte-Domain (name → asset) subsystem.
- **`src/IPFS.*`** — controller/worker for the local IPFS (**Kubo**) node over its
  HTTP API (default `http://localhost:5001/api/v0/`). Its own thread drains the
  Database's IPFS job queue: download (`cat`) DigiAsset content and pin/unpin CIDs
  for the pools. Offers async (callback / promise) and sync (`downloadFile`,
  `isPinned`, `getSize`) APIs, plus pure helpers (SHA256↔CID, CID/URL validation,
  a "known lost" CID skip list) and `getPeerId()` — this node's dialable
  multiaddr, which the pool server uses to reach it.
- **`src/PermanentStoragePool/**`** — the pinning/hosting layer:
  - `PermanentStoragePoolList.*` — owns one instance of every known pool, indexed
    (**pool 0 = local**, others follow). When ChainAnalyzer finds new asset
    metadata, `processNewMetaData()` asks each pool if the tx is theirs and
    schedules an IPFS download whose completion callback pins the wanted files.
  - `pools/local.*` — the **local** pool (`psp0`): no server, no cost. Records
    opt-in CIDs and bad-asset ids in a small `local.db`. Content survives only as
    long as this node runs.
  - `pools/mctrivia.*` — the **networked DigiStamp pool** (`psp1`). Talks to a
    pool server over HTTP (`psp1server`, default `https://pool.digistamp.co`).
    Two threads: `keepAliveTask` (stay visible on the node map) and
    `permanentFetcherTask` (walk `/permanent/<page>.json`, pin every listed CID).
    **Historical note carried in the code:** the original on-chain fee-matching
    payment protocol was never deployed server-side, so `serializeMetaProcessor`
    is dead and the `/list` payout endpoints have returned HTTP 500 since ~2024.
    The **permanent-list pinning is the part that still does useful work.**
  - `PermanentStoragePoolMetaProcessor.*` — decides, per file in an asset, whether
    to pin it.

### Query surfaces

- **`src/RPC/**`** — JSON-RPC-over-HTTP **API server** (`RPC::Server`) on
  `rpcassetport`. HTTP Basic Auth against `rpcuser`/`rpcpassword`; each call is
  either served by an in-process handler under **`RPC/Methods/`** (e.g.
  `listassets`, `getassetdata`, `getassetholders`, `listunspent`, `send`,
  `getpsp`, …) or, if unknown, **forwarded to DigiByte Core** via `sendcommand`.
  `RPC::Cache` memoizes block-stable responses. Uses its own boost::asio
  io_context + thread pool.
- **`src/WebServer.*`** + **`web/`** — optional static web helper/docs UI (port
  `webport`, default 8090).
- **`cli/main.cpp`** — `DigiAssetWindows-cli.exe`, a thin command-line RPC client:
  coerces argv into JSON types, forwards the command to the running node over
  JSON-RPC (reading `config.cfg`), prints the result, with friendly messages for
  "node down" and "forbidden by config". The operator's "poke the node" tool.
- **`src/ConsoleDashboard.*`** — the in-place VT100 node dashboard (sync %,
  peers, IPFS jobs, PSP health, log tail, quit key).

### Support / infrastructure

`Config.*` (the `key=value` `config.cfg` reader/writer), `Log.*`, `Threaded.*`
(worker-thread base used by ChainAnalyzer & IPFS), `UniqueTaskQueue.*`,
`CurlHandler.*`, `NodeStats.*`, `KYC.*`, `Base58/BitIO/Blob/serialize/utils`,
and the bundled `sqlite3.c` / jsoncpp / libjson-rpc-cpp. Note the historical
**Boost stub trap**: `src/boost/asio.hpp` is a no-op stub — real code includes
specific `boost/asio/*` sub-headers and links real Boost from `packages/`.

---

## 3. The two deployables

Both build from this repo and ship side-by-side in each release.

### A. The node — `DigiAssetWindows.exe` (+ `DigiAssetWindows-cli.exe`)

Everything in §2 above. This is what an ordinary user runs. It indexes the chain
into `chain.db`, pins DigiAsset content via IPFS/Kubo, participates in the
Permanent Storage Pools, and exposes RPC/web/CLI. Installed and kept healthy by
`setup-digiasset.ps1`; runtime helpers in `node/` (`monitor-node.ps1`,
`stop-node.ps1`, `update-binaries.ps1`).

### B. The pool server — `DigiAssetPoolServer.exe` + Caddy website

Under **`pool/`**. An **optional** companion that lets an operator run their own
Permanent Storage Pool implementing mctrivia's wire protocol (since mctrivia's
own server returns 500s on payout endpoints). Only pool operators run it.

- **`pool/main.cpp`** — reads `pool.cfg`, opens `pool.db`, binds the listen
  socket (default port **14028**), runs a one-time **first-run snapshot** that
  imports mctrivia's current `/permanent/0..N.json` so existing clients work
  immediately, then starts the HTTP server + verifier + dashboard.
- **`pool/PoolServer.*`** — minimal boost::asio HTTP server. Routes the pool
  wire protocol: `GET /permanent/<page>.json`, `GET/POST /list/<floor>.json`
  (register payout address; reports honest payout-enabled state),
  `POST /keepalive`, `GET /nodes.json`, `GET /map.json`, `GET /bad.json`,
  `GET /pool/stats.json` (donation/treasury balances + geolocated node points).
- **`pool/PoolDatabase.*`** — SQLite `pool.db`: registered nodes, permanent asset
  list, payout ledger shell, pool config.
- **`pool/PoolVerifier.*`** — background thread that dial-back-verifies registered
  nodes are reachable via the local IPFS `swarm connect` API (~every 60s).
- **`pool/PoolDashboard.*`** — operator TUI (port, nodes, asset count,
  pending/paid totals, uptime; operator menu keys).
- **Payouts are OFF by default** (`poolpayouts=0`): registration works, no DGB
  moves, and clients honestly show "registered (no payouts yet)". Automated payout
  distribution via Core RPC is a future phase.
- **`pool/deploy/`** — the public-facing web front:
  - `Caddyfile` — Caddy reverse proxy: serves the static landing page (`site/`)
    at `/`, proxies the pool API routes to `127.0.0.1:14028`, and auto-obtains a
    Let's Encrypt TLS cert. Placeholders (`DOMAIN`, `SITE_ROOT`, `POOLPORT`) are
    filled by `setup-caddy.ps1`.
  - `site/` — static landing page (includes the Leaflet world map of nodes).
  - `setup-caddy.ps1`, `start-digistamp.ps1`, `backup-digistamp.ps1`.

`setup-pool.ps1` (repo root) is the one-command installer for the pool box.

---

## 4. Config & paths

Two top-level directories, kept deliberately separate:

### `C:\DigiByte\` — the wallet & blockchain (DigiByte Core)

- `digibyte.conf` — lives **directly** in `C:\DigiByte` (NOT in Data). Holds
  `server=1`, `rpcuser`/`rpcpassword` (must match `config.cfg`), `rpcport`
  (14022), dbcache, etc. Written by the installer.
- `C:\DigiByte\Data\` — the actual blockchain data dir: `blocks\`, `chainstate\`,
  `wallets\`. This is what the fast-sync snapshot restores.

### `C:\DigiAssetWindows\` — the node

- `DigiAssetWindows.exe`, `DigiAssetWindows-cli.exe`
- `config.cfg` — the node's settings (see `example.cfg` for the fully documented
  reference). Simple `key=value` lines. Key groups: DigiByte Core RPC creds; this
  node's own RPC/API server (`rpcassetport=14024`, `rpcallow*` policy); chain
  analyzer (`pruneage`, `storenonassetutxo`, `bootstrapchainstate`); the PSP
  settings; IPFS (`ipfspath`); web (`webport`).
- `chain.db` — the SQLite chain index (Database). `local.db` — the local pool's
  opt-in/bad tables.
- IPFS/Kubo data also lives under here.

### `C:\DigiAssetWindows\` on a pool box (additional)

- `pool.cfg` — pool server settings (`poolport=14028`, `pooldbpath`,
  `poolpayouts`, `ipfspath`, donation/RPC info for the stats page).
- `pool.db` — the pool's SQLite state.

### The psp0 vs psp1 model

Every node always has **two** Permanent Storage Pools, addressed by index:

| Index | Name       | Backing                | Cost | Persistence |
|-------|------------|------------------------|------|-------------|
| `psp0` | **local**  | `local.db`, no server  | free | only while this node runs |
| `psp1` | **DigiStamp pool** | HTTP pool server (`psp1server`, default `https://pool.digistamp.co`) | (payments currently unavailable) | as long as the pool + its nodes pin it |

Each pool has `psp<N>subscribe`, `psp<N>payout` (both pools need a payout address
or the node errors), and `psp<N>autoremovebad`. `psp1` additionally has
`psp1server`, `psp1visible` (show on the pool world map), `psp1secret` (per-node
identity, auto-generated — never copy between nodes), and `psp1permanentpage`
(permanent-list cursor). To point a node at a locally-run
`DigiAssetPoolServer.exe`, set `psp1server=http://127.0.0.1:14028`.

---

## 5. Build & release

- **Build system:** CMake (`CMakeLists.txt`), MSVC on Windows. Options:
  `BUILD_CLI`, `BUILD_WEB`, `BUILD_TEST` (off by default); the pool server builds
  automatically on `WIN32 AND MSVC`. C++11 for the node, C++17 for the pool.
  Dependencies (jsoncpp, libjson-rpc-cpp) are built into `*/install` prefixes and
  found via `CMAKE_PREFIX_PATH`; real Boost + SQLite come from `packages/`.
- **Versioning:** upstream version is `MAJOR.MINOR.PATCH` (kept in sync with
  DigiAsset Core), and **`WIN_BUILD`** in `CMakeLists.txt` is the source of truth
  for the Windows port build number (the `win.<N>` release tag). It is substituted
  into `src/Version.h.in` → the generated (gitignored) `src/Version.h`. Bump
  `WIN_BUILD` per release; never hand-edit the generated header.
- **Helper scripts:** `_build_now.bat`, `setup_and_build.bat`,
  `config*.bat`/`install-dependencies.bat` (dependency setup), `run_tests.bat`.
- **One-liner install layout:** `setup-digiasset.ps1` is fetched via
  `iwr <raw.githubusercontent.com>/.../setup-digiasset.ps1` and run with
  `-ExecutionPolicy Bypass`. Modes: **Install** (interactive first run) and
  **Service** (SYSTEM, on boot, self-updating/self-healing). It lays down the
  `C:\DigiByte` and `C:\DigiAssetWindows` trees described in §4, restores the
  fast-sync snapshot, and registers boot/logon scheduled tasks. `setup-pool.ps1`
  is the parallel one-command installer for a pool operator's box. Snapshot
  tooling lives in `snapshots/` (`make-snapshot.ps1`, `seed-digibyte.ps1`), which
  produces the R2-hosted `snapshot.json` + archives that new installs fast-sync
  from.
