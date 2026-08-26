# DigiAsset for Windows

> **This is a Windows port of [DigiAsset Core](https://github.com/DigiAsset-Core/DigiAsset_Core) originally created by [mctrivia](https://github.com/mctrivia).** All core logic, chain analysis, RPC methods, and DigiAsset protocol implementation are their work. This repository only adds Windows (MSVC) build support, platform-specific stubs, and a console dashboard UI.

## ⚡ Quick install (recommended)

**Most people should use this — you do not need to clone or build anything.** On
the Windows PC that will run the node, open **PowerShell as Administrator** (click
Start, type `PowerShell`, right-click **Windows PowerShell**, choose *Run as
administrator*) and paste this single line:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-digiasset.ps1 -OutFile "$env:TEMP\setup-digiasset.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-digiasset.ps1"
```

It installs the whole stack as **GUI apps** — the **DigiByte wallet**, **IPFS
Desktop** (tray icon), and the **DigiAsset node dashboard** — writes every config,
installs the VC++ runtime, opens the firewall, and adds a background task that
**auto-updates** and re-checks health. The apps open when you log in (set up
[Autologon](https://learn.microsoft.com/sysinternals/downloads/autologon) for an
always-on box). It asks only for your DGB payout address. Full walkthrough:
**[NODE-SETUP.md](NODE-SETUP.md)**.

> Building from source is **only for developers** — see [Build on Windows](#build-on-windows)
> below. If you just want to run a node and earn DGB, use the one line above.

**Host a node and earn DGB.** That's the whole thing — run the one-liner above.
Your node hosts DigiAsset content and automatically joins the **DigiStamp pool**
(`pool.digistamp.co`), which verifies it and pays it DGB for hosting. You don't
run or manage a pool — it's already run for you. Full guide: **[NODE-SETUP.md](NODE-SETUP.md)**.

## ⚡ Install a DigiByte Core wallet, seeded (standalone)

Want **just DigiByte Core** — no IPFS, no DigiAsset node, no pool — up and
running near the chain tip instead of syncing for days? This one script does the
whole job: installs DigiByte Core, writes a complete `digibyte.conf`, seeds the
blockchain from the same pre-synced snapshot the node installer uses, opens the
firewall for P2P, starts the wallet, **creates a wallet for you, and offers to
encrypt it**. In an **Administrator PowerShell**:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/install-digibyte.ps1 -OutFile "$env:TEMP\install-digibyte.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\install-digibyte.ps1"
```

It self-elevates, so you can also just right-click > Run with PowerShell. Useful
switches: `-DataDir "$env:APPDATA\DigiByte"` for the stock DigiByte layout,
`-Headless` to run `digibyted` instead of the GUI wallet, `-Lean` to skip the
optional service indexes, `-SkipSeed` to sync from genesis, and `-Force` for a
fully unattended run. Re-running is safe — an existing `digibyte.conf` is topped
up rather than overwritten (your RPC password survives), and an existing
blockchain is left alone.

**About the wallet.** Modern DigiByte Core (Bitcoin Core base) does *not* create a
wallet on its own, so a fresh install has nothing to receive into. This script
creates one once RPC is up, then offers to encrypt it — a passphrase is needed to
**spend**, while receiving still works without one. Answer `n` to skip and do it
later in DigiByte-Qt (*Settings > Encrypt Wallet*). **Back up `wallet.dat` and keep
a copy off the machine**; there is no recovery if the disk dies or you lose the
passphrase.

> **Seeding** reads the published manifest at run time, so you always get the
> newest snapshot rather than a pinned one — currently **DigiByte 9.26.5, block
> ~24,045,000** (published Aug 2026). It downloads ~34 GB and needs roughly
> **86 GB free** while it unpacks; the script checks and warns before committing
> to the download. Every archive is SHA256-verified, and any failure falls back to
> a normal sync rather than leaving a half-written data directory.

### Already have DigiByte Core installed?

Then you only need the **seeding** half. This older script skips the install and
just downloads + verifies + extracts the snapshot into an existing data
directory. It **prompts you for the location and sanity-checks it first**, so you
can't accidentally unpack the chain into the wrong folder. In an
**Administrator PowerShell**:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/snapshots/seed-digibyte.ps1 -OutFile "$env:TEMP\seed-digibyte.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\seed-digibyte.ps1"
```

> Prefer a release-pinned copy? The same script also ships as a release asset:
> `https://github.com/chopperbriano/DigiAssetWindows/releases/latest/download/seed-digibyte.ps1`

Start DigiByte Core once and close it (so the data directory exists), then run
the line above. It offers a detected default (`%APPDATA%\DigiByte` for stock
DigiByte Core, `C:\DigiByte\data` for the DigiAsset layout), warns and asks for
confirmation if the folder doesn't look like a DigiByte data directory, then
downloads + extracts. Pass `-DataDir "$env:APPDATA\DigiByte"` to skip the prompt,
or `-Force` for unattended use.

> Which one do I want? **Nothing installed yet → `install-digibyte.ps1`**
> (it installs *and* seeds). **DigiByte already installed → `seed-digibyte.ps1`.**
> **Want the full DigiAsset node → `setup-digiasset.ps1`**, which already does
> all of this for you.

## Repository layout
| Path | What |
|---|---|
| `setup-digiasset.ps1` | **Node installer — the one-liner. This is what you run.** |
| `install-digibyte.ps1` | Standalone **DigiByte Core only**: install + configure + seed. No IPFS/node/pool. |
| `node/` | Node operator helpers: `monitor-node.ps1`, `stop-node.ps1`, `update-binaries.ps1`. |
| `snapshots/` | Fast-sync tooling: `make-snapshot.ps1` (create), `seed-digibyte.ps1` (consume — prompts for + validates the DigiByte data folder). |
| `pool/`, `setup-pool.ps1` | Pool-server code + installer. **Operator-only — you don't need these**; the DigiStamp pool is already run for you. |
| `src/`, `cli/` | The node/analyzer C++ and command-line client. See **[ARCHITECTURE.md](ARCHITECTURE.md)**. |
| `example.cfg` | Fully-commented `config.cfg` reference. |

Paths on an installed machine: DigiByte in `C:\DigiByte` (blockchain in `C:\DigiByte\Data`, `digibyte.conf` in `C:\DigiByte`); the node + pool in `C:\DigiAssetWindows`.

## Table of Contents
1. [How It Works (Architecture)](#how-it-works-architecture)
2. [Build on Windows](#build-on-windows)
3. [Optional Build Targets](#optional-build-targets)
4. [Install DigiByte](#install-digibyte)
5. [Install IPFS](#install-ipfs)
6. [Configure DigiAsset for Windows](#configure-digiasset-for-windows)
7. [Permanent Storage Pools & Getting Paid](#permanent-storage-pools--getting-paid)
8. [Documentation](#Documentation)
9. [Other Notes](#other-notes)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## How It Works (Architecture)

DigiAsset for Windows is the "brain" that ties together **three separate programs**. To
run a full node you install and run all three. There is also a **fourth,
optional** program (`DigiAssetPoolServer.exe`) that only pool operators need.

### The three things you install

| # | Program | What it provides | Why DigiAsset for Windows needs it |
|---|---|---|---|
| 1 | **DigiByte Core wallet** (`digibyted`) | The DigiByte blockchain, a JSON-RPC interface, and a wallet | DigiAsset for Windows reads every block and transaction from it over RPC to find DigiAsset issuances/transfers. For pool payouts it also calls the wallet's `sendtoaddress`. |
| 2 | **IPFS Desktop / kubo** | Content-addressed file storage (the actual asset images + metadata) | DigiAssets store their media on IPFS. DigiAsset for Windows **pins** those files so they stay available, via IPFS's HTTP API. |
| 3 | **DigiAsset for Windows** (this repo) | The chain analyzer, asset database, RPC server, web UI, and dashboard | The coordinator: reads the chain from DigiByte Core, decodes DigiAsset data, and pins the right files on IPFS. |

DigiByte Core and IPFS are external programs you download and install.
DigiAsset for Windows is what you build here (or download from [Releases](https://github.com/chopperbriano/DigiAssetWindows/releases)).

### The executables this repo builds

| Executable | Needed? | What it does |
|---|---|---|
| **`DigiAssetWindows.exe`** | **Required** | The node itself. One process runs the sync engine, asset database, JSON-RPC server, the built-in web UI (http://localhost:8090), and the console dashboard. **This is the program you run.** |
| `DigiAssetWindows-cli.exe` | Optional | A command-line client that sends RPC commands to a running `DigiAssetWindows.exe` (query assets, trigger actions). Useful for scripting; the web UI covers most needs. |
| `DigiAssetPoolServer.exe` | Optional — **pool operators only** | A standalone pool server that pays node operators for hosting. Most users never run this. See [Permanent Storage Pools & Getting Paid](#permanent-storage-pools--getting-paid). |

You do **not** need a separate web-server exe — the web UI is built into
`DigiAssetWindows.exe`.

### How data flows

```
        reads blocks/txs (RPC :14022)            pins/serves files (API :5001)
 DigiByte Core  ──────────────────────►  DigiAsset for Windows  ───────────────►  IPFS (kubo)
 blockchain + wallet                     DigiAssetWindows.exe                        file storage
        ▲                                     │      │
        │  sendtoaddress (pool payouts)       │      └──► web UI  http://localhost:8090
        └─────────────────────────────────────┘
```

### Ports at a glance

| Port | Program | Purpose |
|---|---|---|
| 14022 | DigiByte Core | JSON-RPC — DigiAsset for Windows reads the chain and sends payouts here |
| 12024 | DigiByte Core | P2P network (blockchain sync) |
| 5001 | IPFS (kubo) | HTTP API (pin / cat / findprovs) |
| 4001 | IPFS (kubo) | Swarm P2P — other peers connect here. **Forward this if you want pool payouts** (see below) |
| 8090 | DigiAsset for Windows | Built-in web UI |
| 14028 | DigiAssetPoolServer | Pool server HTTP (operators only) |

## Build on Windows

This fork builds a Windows version with Visual Studio and MSVC in the main branch, with upstream tracking in the 'upstream-master' branch. Upstream changes from [DigiAsset-Core/DigiAsset_Core](https://github.com/DigiAsset-Core/DigiAsset_Core) are merged periodically.

Most dependencies (libcurl, OpenSSL, SQLite3, libjsonrpccpp) are replaced by vendored source files or Windows-native stubs (WinHTTP), so no vcpkg or external package manager is needed beyond the jsoncpp and libjson-rpc-cpp subprojects that are already in the repo.

Note: If you want to skip building from source, download the pre-built binaries from the [Releases](https://github.com/chopperbriano/DigiAssetWindows/releases) page. You will still need to install IPFS Desktop and DigiByte Core wallet as described below. Run `DigiAssetWindows.exe` from a cmd prompt — the web server is built in (no separate exe needed).

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

You can enable the CLI, Web server, and test suite by passing CMake options:

```cmd
cmake .. -DBUILD_CLI=ON -DBUILD_WEB=ON -DBUILD_TEST=ON
```

| Target | Binary | Description |
|---|---|---|
| `BUILD_CLI` | `DigiAssetWindows-cli.exe` | Command-line RPC client |
| `BUILD_WEB` | `digiasset_core-web.exe` | Standalone web server (legacy, now built into main exe) |
| `BUILD_TEST` | `Google_Tests_run.exe` | Google Test suite |

`DigiAssetPoolServer.exe` (the optional pool server) is **built automatically on
Windows/MSVC** — no flag needed. It lands in `build\pool\Release\`. You only run
it if you operate your own Permanent Storage Pool; see
[Permanent Storage Pools & Getting Paid](#permanent-storage-pools--getting-paid).

### Performance Tuning

For faster initial blockchain sync, add to `config.cfg`:

```
verifydatabasewrite=0
```

This disables SQLite write verification (fsync), significantly reducing sync time.


## Install DigiByte

> **v9.26.5 is a hard minimum, not a recommendation.** DigiAsset for Windows refuses to
> sync against an older node and exits on startup (`MINIMUM_NODE_VERSION` in
> `src/DigiByteCore.h`). Two independent reasons, both from upstream's readme:
>
> **DigiDollar.** It activated on mainnet at block **23,869,440** and this node indexes
> DigiDollar balances, collateral vaults and the oracle DGB/USD rate. DigiDollar activated
> as a BIP9 *soft* fork, so a v8.x node still follows the same chain and still answers RPC —
> it simply cannot validate DigiDollar, so anything it reports about those transactions is
> untrustworthy. v9.26.4 was the first release that works with `prune=`, and v9.26.5 fixed a
> multi-minute oracle scan hang at node startup.
>
> **The Groestl incident.** On 28 June 2026, from block 23,751,096, an attacker exploited a
> consensus check accidentally dropped during the v8 Bitcoin Core rebase and mined the retired
> Groestl algorithm at floor difficulty. v8/v9 accepted those blocks; v7.17.3 and older
> rejected them and forked away. The fix (`algolock`, buried at 23,869,440) rejects retired
> algorithms going forward but **grandfathers the exploit blocks already in the chain** — so
> canonical DigiByte history permanently contains Groestl blocks that old nodes reject
> outright. Those nodes cannot follow the current chain at all.
>
> One visible consequence for `algostats`: `ALGO_GROESTL` is algo id **2**, empty from 2019
> until June 2026. For windows covering the incident, index 2 of the `algo` array is populated
> rather than `null`. That is correct data, not a bug.

Download and install DigiByte Core Wallet v9.26.5 (the version the installer uses). https://github.com/DigiByte-Core/digibyte/releases/download/v9.26.5/digibyte-9.26.5-win64-setup.exe
Install to the default locations, unless you need to change the location on your hard drive. Then add the following lines to the digibyte.conf file.

```
rpcuser=user
rpcpassword=pass11
rpcbind=127.0.0.1
rpcport=14022
whitelist=127.0.0.1
rpcallowip=127.0.0.1
listen=1
server=1
txindex=1
deprecatedrpc=addresses
addnode=191.81.59.115
addnode=175.45.182.173
addnode=45.76.235.153
addnode=24.74.186.115
addnode=24.101.88.154
addnode=8.214.25.169
addnode=47.75.38.245
```


## Install IPFS

Download and install IPFS Desktop from https://github.com/ipfs/ipfs-desktop/releases

After installation, verify the IPFS API is running. The line "RPC API server listening on" shows the port (usually 5001). You can access the IPFS web UI at http://localhost:5001/webui in your browser.

## Configure DigiAsset for Windows

The first time you run DigiAsset for Windows it will ask you several questions to set up your config file. Run `DigiAssetWindows.exe` from a cmd prompt:

```cmd
DigiAssetWindows.exe
```

The single executable runs both the sync engine and the web UI server. The console displays a live dashboard with sync progress, service status, asset count, and a link to the web UI (default: http://localhost:8090/).

This will create config.cfg — the wizard creates only the basic config. For a full list of config options see example.cfg.

Make sure DigiAsset for Windows is running correctly and then press ctrl+c to stop it and continue with instructions.

NOTE: For peer connectivity and pool payout verification, forward these inbound
ports to this machine:

```bash
Inbound TCP:4001    # IPFS swarm — lets peers (and the pool) reach your node
Inbound TCP:12024   # DigiByte P2P — inbound blockchain peers (optional)
```

⚠️ **Do NOT expose port 5001 to the internet.** 5001 is the IPFS HTTP API and is
**unauthenticated** — anyone who can reach it controls your IPFS node (add/remove
pins, read files, reconfigure it). Keep 5001 bound to localhost only. The port
that needs to be reachable for hosting/payouts is **4001** (the IPFS swarm port),
not 5001.

## Permanent Storage Pools & Getting Paid

DigiAssets keep their files on IPFS, but IPFS only holds a file while **someone
pins it**. **Permanent Storage Pools (PSPs)** are the incentive layer that keeps
those files alive: when an asset is created, the creator pays a small fee, and
that fee is shared over time among the node operators who agree to permanently
host ("pin") the asset's files. In short — **run a node, host the files, earn
DGB.**

There are two roles. Most people are node operators; only pool runners are pool
operators.

### Node operator — you want to earn DGB for hosting

1. Run DigiByte Core + IPFS + `DigiAssetWindows.exe` as described above.
2. In `config.cfg`, subscribe to a pool and set your payout address (the `#` is
   the pool number; see `example.cfg` for all keys):
   ```
   psp0subscribe=1
   psp0payout=<your DGB address>          # pool 0 = local pool (no server); needs a payout too
   psp2server=https://pool.digistamp.co   # pool 2 = DigiStamp, the pool you're joining
   psp2subscribe=1
   psp2payout=<your DGB address>
   ```
   (Pool 1 was MCTrivia's PSP — now **deprecated**; new nodes join pool 2,
   DigiStamp. Existing nodes still on `psp1` keep working.)
   Both pools need a payout address or the node errors with "Could not generate
   new PSP payout address". A `_label` value auto-creates an address, but only if
   a DigiByte wallet is loaded — use a real address to be safe. The installer sets
   all of this for you.
3. **Strongly recommended: forward IPFS port 4001** (or set `Addresses.Announce`
   in kubo). The pool verifies you're actually hosting before it pays you.
   Forwarded nodes verify instantly and reliably. Nodes behind NAT can still
   qualify through a fallback provider-record check, but it's slower and less
   reliable — so forwarding the port is the difference between "paid promptly"
   and "maybe paid eventually." Details in **[pool/README.md](pool/README.md)**.
4. Watch the dashboard's **Payment** row for your status (`registered (no payouts
   yet)` → the pool hasn't enabled payouts; `active` → it has).

> **About pools.** Matthew Cornelisse (mctrivia) created the original DigiAsset
> Permanent Storage Pool at `ipfs.digiassetx.com` (pool #1) — the design and
> protocol that make all of this possible, and huge credit for it. Building on
> that foundation, this fork ships its own pool server so the **community can run
> their own paying pools** (like DigiStamp) with full control over registration
> and payouts. Point your node at whichever pool you like via `psp2server`
> (mctrivia's original pool #1 is deprecated; DigiStamp is pool #2).

### Pool operator — you want to run a pool and pay hosts

Run **`DigiAssetPoolServer.exe`**. It is a self-contained server that:

- serves the canonical permanent-asset list (`/permanent/<page>.json`),
- accepts node registrations (peerId + payout address) and keepalives,
- **verifies** that each registered node is really hosting the content (direct
  IPFS dial, with a NAT-tolerant DHT fallback), and
- **pays** verified nodes from your DigiByte Core wallet, on your explicit
  command, with a spend budget and a per-period guard.

It's a *separate* program from `DigiAssetWindows.exe` on purpose: it links its own
minimal server/dashboard so it can run independently. Step-by-step setup — the node
stack, `pool.cfg`, HTTPS via Caddy, funding, and the `[P]`/`[E]` payout flow — is in
**[POOL-SETUP.md](POOL-SETUP.md)**; the full config/API reference is in
**[pool/README.md](pool/README.md)**.

**Running more than one pool?** Independent pools (each with its own wallet and
treasury) can be **peer-aware**: they mirror the permanent list, show one combined
world map, coordinate payouts, and **auto-discover each other** across the network
— via a seed + gossip *and* on-chain DigiByte announcements — so anyone who stands
up a pool on any domain joins the network automatically. Stand up + pair a new pool
in one command with `pool/deploy/provision-peer-pool.ps1`; see
**[POOL-SETUP.md](POOL-SETUP.md)**.

## Documentation

- **[CHEATSHEET.md](CHEATSHEET.md)** — one page, every script: pick your role (node / pool / peer pool / snapshot), copy the command. Start here.
- **[NODE-SETUP.md](NODE-SETUP.md)** — run a node and get paid: one-line install, ports, monitoring, updating.
- **[POOL-SETUP.md](POOL-SETUP.md)** — run a Permanent Storage Pool: setup, payouts, and running multiple **peer-aware, auto-discovering pools** as one network.
- **[pool/README.md](pool/README.md)** — pool reference: every `pool.cfg` key, the verification model, the payout flow.
- **[pool/deploy/README.md](pool/deploy/README.md)** — the deploy toolkit: Caddy/HTTPS, `provision-peer-pool.ps1`, `add-peer.ps1`, `verify-peers.ps1`, `diagnose-website.ps1`, `start-digistamp.ps1`, backups, `update-pool.ps1`.
- **[snapshots/README.md](snapshots/README.md)** — fast-sync snapshots (skip the multi-day DigiByte sync).
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — how the pieces fit together · **[CHANGELOG.md](CHANGELOG.md)** — release history.
- **[Releases](https://github.com/chopperbriano/DigiAssetWindows/releases)** — the `.exe`s + scripts for each build.

### The Node Console (http://localhost:8090)

Built into `DigiAssetWindows.exe` (no separate exe) — once the node is running,
open **http://localhost:8090** in a browser. It's a live, loopback-only console:

- **Dashboard** — sync progress + ETA, DigiAssets indexed + latest issuances,
  IPFS serving, permanent-storage coverage, **DigiByte network + wallet** (balance,
  peers, verification %, difficulty), **pool status + payout**, and service health.
  Same animated look as the pool site; refreshes every few seconds.
- **RPC Reference** — a searchable, categorized browser for every RPC method
  (DigiAssets methods documented from source), with a "how to call these" primer
  and copy-paste CLI/curl examples.

Keep port 8090 private — never forward it.

## Credits

This project is a Windows port of **[DigiAsset Core](https://github.com/DigiAsset-Core/DigiAsset_Core)** by **[mctrivia](https://github.com/mctrivia)** and contributors. The core DigiAsset protocol implementation, chain analyzer, RPC interface, database schema, and all blockchain logic are entirely their work.

This fork adds only:
- Windows/MSVC build system and platform stubs (WinHTTP, OpenSSL stubs, vendored SQLite3)
- Console dashboard UI (VT100-based TUI)
- Embedded web server (no separate exe)
- Sync performance optimizations (prefetch pipeline, UTXO caching)

## Generating a Bootstrap Image

New installs can download a prebuilt database from IPFS instead of syncing the whole chain
themselves (`bootstrapchainstate` in the config).  `--bootgen` builds that image:

```
./digiasset_core --bootgen
```

It syncs normally and shuts itself down as soon as it reaches the chain tip.  New blocks
keep arriving, so the image may end up a block or two behind by the time it is published —
that is fine, a new node just syncs the remainder itself.  The RPC server and event stream
stay off for the whole run so nothing outside the process can write to the database while
the image is being made.

On shutdown the database is folded out of WAL mode and vacuumed, so what is left on disk is
a single self contained file with no `chain.db-wal` or `chain.db-shm` beside it — safe to
add to IPFS as is.  Because a normal run keeps the database in WAL mode, copying `chain.db`
out from under a running daemon does *not* give you a usable image; use this flag.

Vacuuming temporarily needs about as much free disk space as the database itself.  The
synced block height is printed at the end; put it, along with the CID you get from
`ipfs add`, into `officialBootstrap` in `src/main.cpp`, and move the CID it replaces into
`oldBootstrapCIDs` so existing nodes unpin it.

## Other Notes

- If submitting pull requests please utilize the .clang-format file to keep things standardized.
- Upstream changes are tracked on the `upstream-master` branch and merged into `master` periodically.

---

