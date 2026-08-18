# DigiAsset Core

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

DigiAsset Core is a DigiAsset processing node for the DigiByte blockchain.  It decodes and
tracks all DigiAsset transactions, serves asset data over JSON-RPC, and (when connected to a
wallet enabled DigiByte Core) can create, send and track DigiAssets.

Building this project produces up to 4 binaries (all placed in `bin/`):

| Binary | What it is |
|---|---|
| `digiasset_core` | The main daemon.  Syncs against DigiByte Core and serves the JSON-RPC api (default port 14024) |
| `digiasset_core-cli` | Command line interface to the daemon.  Any RPC method (including all DigiByte wallet methods) can be called: `./digiasset_core-cli getwalletbalances` |
| `digiasset_core-web` | Documentation web server (default port 8090) |
| `digiasset_core-qt` | Graphical interface (sync status, balances, sending assets, creating assets with optional royalty rules, burning/reissuing, wallet history) |

## Table of Contents
1. [Requirements](#requirements)
2. [Install on Ubuntu](#install-on-ubuntu)
3. [Install on macOS](#install-on-macos)
4. [Install on Windows](#install-on-windows)
5. [Configure DigiAsset Core](#configure-digiasset-core)
6. [Set DigiAsset Core to Run at Boot](#set-digiasset-core-to-run-at-boot)
7. [Upgrading DigiAsset Core](#upgrading-digiasset-core)
8. [Documentation](#documentation)
9. [DigiDollar](#digidollar)
10. [Event Stream](#event-stream)
11. [Generating a Bootstrap Image](#generating-a-bootstrap-image)
12. [Other Notes](#other-notes)
13. [Running the Tests](#running-the-tests)
14. [Special Thanks](#special-thanks)

## Requirements

- **DigiByte Core v9.26.5 or newer** with `txindex=1` (default since v9.26.3).  Wallet support
  must be enabled (it is in the official release binaries) if you want to create or send assets —
  the `issueasset`/`sendasset`/`getwalletbalances` methods and the wallet RPC passthrough need it.

  v9.26.5 is a hard minimum, not a recommendation.  DigiAsset Core refuses to sync against an
  older node and exits on startup.  There are two independent reasons:

  **DigiDollar.**  DigiDollar activated on mainnet at block **23,869,440** (17 July 2026) and
  DigiAsset Core indexes DigiDollar balances, collateral vaults and the oracle DGB/USD rate.
  DigiDollar activated as a BIP9 *soft* fork, so a v8.x node still follows the same chain and
  still answers RPC — it simply cannot validate DigiDollar, so anything it reports about those
  transactions is untrustworthy.  v9.26.4 was the first release that works with `prune=`, and
  v9.26.5 fixed a multi-minute oracle scan hang at node startup.

  **The Groestl incident.**  On 28 June 2026, starting at block 23,751,096, an attacker
  exploited a consensus check that had been accidentally dropped during the v8 Bitcoin Core
  rebase and mined the retired Groestl algorithm at floor difficulty.  v8/v9 software accepted
  those blocks while v7.17.3 and older rejected them and forked onto a separate chain.  The fix
  (`algolock`, backstop height 23,808,000, buried at 23,869,440) rejects retired-algorithm
  blocks going forward but **grandfathers the exploit blocks already in the chain**.  Canonical
  DigiByte history therefore permanently contains Groestl blocks that v7.17.3 and older nodes
  reject outright.  Those nodes cannot follow the current chain at all and must reindex or
  resync after upgrading.

  Verified against canonical chain data rather than taken from the incident report: across a
  351 block sample of the attack window, 16% of blocks are Groestl, the first is at exactly
  23,751,096, and there are none before the attack or after `algolock`.  The two coinbase
  payout addresses (`dgb1qy5epvfs535a96tygn945a3a85lauh3ddu9v63y` and
  `D8S5JWaCrpFsryGG1c9AzWKhbS7e7VZ4r8`) received 392,616.56 DGB across 1,514 outputs and have
  since been emptied.  Claims in the upstream release notes about whether funds were taken or
  transactions reversed concern orphaned blocks, which cannot be checked from canonical
  history — this project takes no position on them.

  One visible consequence for `algostats`: `ALGO_GROESTL` is algo id **2**, which was empty for
  every block between 2019 and June 2026.  For time windows covering the incident, index 2 of
  the `algo` array is populated instead of `null`.  That data is correct — do not treat a
  non-null index 2 as a bug.

- **IPFS (kubo)** running on the same machine (asset metadata storage).
- **cmake 3.24+** and a C++14 capable compiler.
- Roughly 100GB of disk space for the DigiByte chain plus the DigiAsset database.

## Install on Ubuntu

Tested on Ubuntu 22.04 LTS.  Ubuntu 20.04 works for the main app but the google tests don't
compile there.

### Increase swap size (low RAM machines only)

DigiByte Core can crash during sync on machines with little RAM.  If your machine has less
than 8GB, increase swap to 8GB:

```bash
sudo swapoff /swap.img
sudo dd if=/dev/zero bs=1M count=8192 oflag=append conv=notrunc of=/swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
sudo swapon --show
sudo nano /etc/fstab
```

place the following at the end (if swap.img is already there replace it)

```
/swapfile       none    swap    sw      0       0
```

### Install DigiByte

```bash
wget https://github.com/DigiByte-Core/digibyte/releases/download/v9.26.5/digibyte-9.26.5-x86_64-linux-gnu.tar.gz
tar -xf digibyte-9.26.5-x86_64-linux-gnu.tar.gz
rm digibyte-9.26.5-x86_64-linux-gnu.tar.gz
mkdir .digibyte
nano .digibyte/digibyte.conf
```

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
addnode=191.81.59.115
addnode=175.45.182.173
addnode=45.76.235.153
addnode=24.74.186.115
addnode=24.101.88.154
addnode=8.214.25.169
addnode=47.75.38.245
```

to get digibyte to run on boot do the following

```bash
sudo nano /etc/systemd/system/digibyted.service
```

```
[Unit]
Description=DigiByte's distributed currency daemon
After=network.target

[Service]
User=<your-username>
Group=<your-username>

Type=forking
PIDFile=/home/<your-username>/.digibyte/digibyted.pid
ExecStart=/home/<your-username>/digibyte-9.26.5/bin/digibyted -daemon -pid=/home/<your-username>/.digibyte/digibyted.pid \
-conf=/home/<your-username>/.digibyte/digibyte.conf -datadir=/home/<your-username>/.digibyte

Restart=always
PrivateTmp=true
TimeoutStopSec=60s
TimeoutStartSec=2s
StartLimitInterval=120s
StartLimitBurst=5

[Install]
WantedBy=multi-user.target
```

replace <your-username>

Enable and start the service

```bash
sudo systemctl enable digibyted.service
sudo systemctl start digibyted.service
```

### Install dependencies

```bash
sudo apt update
sudo apt upgrade
sudo apt-get install cmake libcurl4-openssl-dev libjsoncpp-dev golang-go libjsonrpccpp-dev libjsonrpccpp-tools libsqlite3-dev build-essential pkg-config zip unzip libssl-dev
sudo apt install libboost-all-dev
```

If you want to build the GUI also install Qt:

```bash
sudo apt-get install qtbase5-dev libqt5charts5-dev
```

### Update cmake

DigiAsset Core needs cmake 3.24 or newer.  Ubuntu 22.04 ships 3.22 so a manual install is
needed:

```bash
wget https://github.com/Kitware/CMake/releases/download/v3.27.7/cmake-3.27.7-linux-x86_64.sh
chmod +x cmake-3.27.7-linux-x86_64.sh
sudo ./cmake-3.27.7-linux-x86_64.sh --prefix=/usr/local
export PATH=/usr/local/cmake-3.27.7-linux-x86_64/bin:$PATH
nano ~/.bashrc
```

at the end of the file add

```
export PATH=/usr/local/cmake-3.27.7-linux-x86_64/bin:$PATH
```

### Install IPFS

```bash
wget https://dist.ipfs.tech/kubo/v0.22.0/kubo_v0.22.0_linux-amd64.tar.gz
tar -xvzf kubo_v0.22.0_linux-amd64.tar.gz
cd kubo
sudo bash install.sh
ipfs init
ipfs daemon
```

this step will list out a lot of data of importance is the line that says "RPC API server listening on" it is usually
port 5001 note it down if it is not. You can now see IPFS usage at localhost:5001/webui in your web browser(if not
headless).
Press Ctrl+C to stop the daemon

To set IPFS to run on boot:

```bash
cd ~
sudo nano /etc/systemd/system/ipfs.service
```

edit the file to look like this

```
[Unit]
Description=IPFS Daemon
After=network.target

[Service]
ExecStart=/usr/local/bin/ipfs daemon
User=<your-username>
Restart=always

[Install]
WantedBy=multi-user.target
```

replace <your-username>

```bash
sudo systemctl daemon-reload
sudo systemctl enable ipfs.service
sudo systemctl start ipfs.service
```

### Build DigiAsset Core

```bash
git clone -b master --recursive https://github.com/DigiAsset-Core/DigiAsset_Core.git
cd DigiAsset_Core
git submodule update --init --recursive
mkdir build
cd build
cmake ..
cmake --build . -j$(nproc)
cd ../bin
```

Notes:
* Binaries are placed in `DigiAsset_Core/bin/` automatically.
* If you don't want the GUI (or don't have Qt installed) add `-DBUILD_QT=OFF` to the first cmake command.
* To also build the test suite add `-DBUILD_TEST=ON`.
* Other options: `-DBUILD_CLI=OFF`, `-DBUILD_WEB=OFF`.

## Install on macOS

Tested on macOS (Intel and Apple Silicon) with [Homebrew](https://brew.sh).

### Install DigiByte

v9.26.5 ships macOS builds as a zip rather than a dmg, with separate archives per architecture.
Download [digibyte-9.26.5-arm64-apple-darwin.zip](https://github.com/DigiByte-Core/digibyte/releases/download/v9.26.5/digibyte-9.26.5-arm64-apple-darwin.zip)
on Apple Silicon or [digibyte-9.26.5-x86_64-apple-darwin.zip](https://github.com/DigiByte-Core/digibyte/releases/download/v9.26.5/digibyte-9.26.5-x86_64-apple-darwin.zip)
on Intel, then create `~/Library/Application Support/DigiByte/digibyte.conf` with the same
settings as the Ubuntu section above (rpcuser, rpcpassword, rpcport=14022, server=1).
`txindex=1` is the default from v9.26.3 onward, so the line is optional.

### Install dependencies

```bash
brew install cmake jsoncpp libjson-rpc-cpp openssl@3 curl sqlite boost
```

If you want to build the GUI also install Qt (Qt5 and Qt6 both work):

```bash
brew install qt
```

### Install IPFS

```bash
brew install ipfs
ipfs init
brew services start ipfs
```

### Build DigiAsset Core

```bash
git clone -b master --recursive https://github.com/DigiAsset-Core/DigiAsset_Core.git
cd DigiAsset_Core
git submodule update --init --recursive
mkdir build
cd build
cmake .. -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build . -j$(sysctl -n hw.ncpu)
cd ../bin
```

The same `-DBUILD_QT=OFF`/`-DBUILD_TEST=ON` options as the Ubuntu section apply.

## Install on Windows

The daemon (`digiasset_core.exe`) builds natively on Windows with Visual Studio and
vcpkg.  The CLI, web server, and Qt GUI are not yet built natively — if you need those
on Windows, use [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install) with
Ubuntu 22.04 and follow the [Ubuntu instructions](#install-on-ubuntu) inside WSL.

Prebuilt Windows binaries are published on the GitHub Releases page for tagged versions.

To build natively you need [Visual Studio 2022](https://visualstudio.microsoft.com/)
(with the "Desktop development with C++" workload), [git](https://git-scm.com/), and
[CMake](https://cmake.org/).  Then from a "x64 Native Tools Command Prompt":

```bat
git clone -b master --recursive https://github.com/DigiAsset-Core/DigiAsset_Core.git
cd DigiAsset_Core

rem Install dependencies with vcpkg (uses vcpkg.json manifest)
git clone https://github.com/microsoft/vcpkg.git
vcpkg\bootstrap-vcpkg.bat

rem Configure and build the daemon
cmake -B build -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE=vcpkg\scripts\buildsystems\vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows ^
    -DBUILD_TEST=OFF -DBUILD_CLI=OFF -DBUILD_WEB=OFF -DBUILD_QT=OFF
cmake --build build --config Release
```

The binary is written to `build\Release\digiasset_core.exe`.  You will also need a
DigiByte Core node (v9.26.5+) and an [IPFS (kubo)](https://docs.ipfs.tech/install/) node
running, the same as on other platforms.

## Configure DigiAsset Core

The first time you run DigiAsset Core it will ask you several questions to set up your config file.  Run DigiAsset Core using

```bash
./digiasset_core
```

This will create bin/config.cfg the wizard creates only the basic config for a full list of config options see example.cfg

Make sure DigiAsset Core is running correctly and then press ctrl+c to stop it and continue with instructions.

---

## Set DigiAsset Core to run at boot

(Linux only)

```bash
sudo nano /etc/systemd/system/digiasset_core.service
```

```
[Unit]
Description=DigiAsset Core
After=network.target digibyted.service

[Service]
User=<your-username>
Group=<your-username>

Type=simple
ExecStart=/home/<your-username>/DigiAsset_Core/bin/digiasset_core
WorkingDirectory=/home/<your-username>/DigiAsset_Core/bin

Restart=always
PrivateTmp=true
TimeoutStopSec=60s
TimeoutStartSec=2s
StartLimitInterval=120s
StartLimitBurst=5

[Install]
WantedBy=multi-user.target
```

replace <your-username>

Enable and start the service

```bash
sudo systemctl enable digiasset_core.service
sudo systemctl start digiasset_core.service
```

## Upgrading DigiAsset Core

When a new version is available you can upgrade by running the following commands

```bash
cd DigiAsset_Core/bin
./digiasset_core-cli shutdown
sudo systemctl stop digiasset_core.service
cd ..
git pull
git submodule update --init --recursive
cd build
cmake ..
cmake --build . -j$(nproc)
cd ../bin
sudo systemctl start digiasset_core.service
```

---

## Documentation

To access documentation run the digiasset_core-web application then go to http://127.0.0.1:8090/

Highlights:
- Every RPC method has its own documentation page, including the DigiAsset specific
  methods (`issueasset`, `reissueasset`, `sendasset`, `sendmanyassets`, `burnasset`,
  `getwalletbalances`, `getassetdata`, `listassets`, ...).
- `issueasset` supports the full DigiAsset v3 rule set (royalties, KYC, geofencing,
  voting, expiry, deflation, signer approval) via its `rules` parameter.
- `issueasset`, `reissueasset`, `sendasset` and `burnasset` all take a `dryrun` option
  that builds the transaction and returns a full cost breakdown (outputs, storage pool
  fee, estimated miner fee) without broadcasting anything.
- Any method the daemon doesn't recognize is transparently forwarded to the DigiByte Core
  wallet, so the standard DigiByte/Bitcoin RPC api is available through the same port too.

## DigiDollar

DigiAsset Core indexes [DigiDollar](https://github.com/orgs/DigiByte-Core/discussions/319),
the native USD stablecoin that activated on DigiByte mainnet at block **23,869,440**.
Three things are tracked:

- **Balances.**  DigiDollar lives on pay-to-taproot outputs that carry 0 DGB, with the
  amount declared in the transaction's `OP_RETURN`.  Because such an output holds no DGB
  and no DigiAssets, it is invisible to the normal UTXO path when `storenonassetutxo=0`,
  so it gets its own table.  All amounts are in **cents** (100 = $1.00), which is the
  unit the protocol itself uses.
- **Collateral vaults.**  A mint locks DGB in a timelocked taproot vault and issues
  DigiDollar against it; a redemption burns the DigiDollar and releases the DGB.  Tracking
  both sides is what makes total supply and the system collateralization ratio computable.
- **The oracle DGB/USD price.**  Roughly two thirds of blocks carry a MuSig2 signed price
  commitment in the coinbase, as `OP_RETURN OP_ORACLE <0x03> <bundle>`.  Because `OP_ORACLE`
  (`0xbf`) is not push-only, DigiByte Core classifies that output as `nonstandard` rather
  than `nulldata` — DigiAsset Core relabels it `oracle`.  Every block inside a 40 block
  epoch republishes the same value, so only one commitment per epoch is stored.

### RPC

| Method | What it adds |
| --- | --- |
| `getdigidollarinfo` | Current price, circulating supply, locked collateral, collateralization ratio |
| `digidollarstats` | The same figures over time, alongside mint/redeem/transfer counts |
| `getaddressholdings` | Extra `digidollar` key holding the address balance in cents |
| `getwalletbalances` | Extra `digidollar` object alongside `digibyte` and `assets` |
| `gettxout` | Extra `digidollar` field: cents carried by that output |
| `getrawtransaction` | Extra `digidollar` object (type, cents, vault) and per-output `digidollar` |
| `getexchangerates` | Extra entry with `"address": "DigiDollar"`, `"index": 0` |
| `getdgbequivalent` | Accepts `"DigiDollar"` as the address to convert USD at the oracle rate |
| `listaddresshistory` | Now includes transactions that only moved DigiDollar |

Every one of those methods has its documentation page updated under `digiasset_core-web`.

### GUI

`digiasset_core-qt` shows DigiDollar in three places:

- **Balances** — the wallet's DigiDollar balance sits beside the DigiByte balance rather than in
  the asset table, since it is a native balance with no assetIndex, assetId or icon.  It is hidden
  when the wallet holds none.
- **Sync** — a DigiDollar panel with the oracle price, how many of the 35 oracle slots signed it,
  circulating supply, locked collateral and the collateralization ratio.  It says so explicitly
  while a backfill is still running, so partial figures are not mistaken for the real state.  The
  oracle rate also appears among the exchange rates, labelled `USD (DigiDollar oracle)`.
- **History** — transactions that moved DigiDollar are labelled `DigiDollar receive` / `DigiDollar
  send` with the dollar amount, alongside the existing asset labelling.

### Indexing and the first run after upgrading

Set `trackdigidollar=0` in `config.cfg` to skip all of this.

DigiDollar history can only be reconstructed by replaying blocks, so the first run after
upgrading an already synced node **rewinds to block 23,869,440 and re-syncs forward**.
That is a long unattended operation and it is logged as a warning when it starts.  It
happens once.  Turning `trackdigidollar` off and back on again forces it to happen again,
because a gap in DigiDollar history cannot be filled in by later blocks.

A node that has pruned UTXO history past the activation height cannot be rewound that far.
It logs a CRITICAL message and leaves DigiDollar unindexed rather than destroying the sync;
resync from scratch or set `trackdigidollar=0`.  `getdigidollarinfo` reports
`"indexed": false` whenever the figures it returns are incomplete for this reason.

## Event Stream

The daemon pushes events to any TCP client as newline delimited JSON (default port
14025, config keys `eventport`/`eventbind`, see `example.cfg`).  Try it with
`nc 127.0.0.1 14025`:

```
{"event":"newBlock","height":23843354,"blocksBehind":1}
{"event":"assetTransfer","assetIds":["La6xk..."],"txid":"d44d...","height":23843354}
{"event":"balanceChanged","addresses":["dgb1q..."],"txid":"d44d...","height":23843354}
```

Event types: `newBlock` (near chain tip only), `assetIssued`, `assetTransfer`,
`assetBurn`, `balanceChanged`, `digiDollarMint`, `digiDollarTransfer` and
`digiDollarRedeem`.  The three DigiDollar events carry a `cents` field holding the
DigiDollar moved, in cents:

```
{"event":"digiDollarTransfer","cents":200,"addresses":["dgb1p..."],"txid":"08da...","height":24045731}
```
  Writes never block the daemon — clients that fall
behind are disconnected, so treat the stream as a wake-up signal and re-query the RPC
api for authoritative state.  The stream has no authentication and therefore only
listens on localhost unless you explicitly set `eventbind`.

The daemon shuts down cleanly on `ctrl+c`, SIGTERM or the `shutdown` RPC method
(database flushed and closed before exit).

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

---

## Running the Tests

`./build_and_test.sh` does everything on Ubuntu: installs dependencies, initialises the
googletest submodule, builds, and runs the suite.  To do it by hand:

```bash
cd build
cmake .. -DBUILD_TEST=ON
cmake --build . -j$(nproc)
cd ../bin          # required - tests resolve ../tests/testFiles/ and config.cfg from the cwd
./Google_Tests_run
```

`-DBUILD_TEST=ON` needs the googletest submodule (`git submodule update --init --recursive`).

### Offline vs full

The suite has two tiers, and this matters: **44 tests fail rather than skip when their
dependencies are absent.**  That is deliberate — see the warning at the top of
`tests/RPCMethods.h` — but it means "44 failures" is the normal state on a machine without a
node, and a real regression would hide in the noise.

To run only the tests that need nothing external:

```bash
./Google_Tests_run --gtest_filter='-RPCMethodsTest.*:DigiByteCore.*:DigiAsset.getStrCount:PermanentStoragePool.mctrivia_allAddressesRecognized'
```

That should be **fully green**.  If anything fails there, it is a real regression.
`build_and_test.sh` picks this mode automatically when it cannot reach a node.

For the full suite you need a synced DigiByte Core *and* IPFS running.  39 of the 44 tests
depend on `tests/testFiles/rpcTest.db`, which is generated by
`DigiAssetTransaction.existingAssetTransactions` — run that one first, then the rest:

```bash
./Google_Tests_run --gtest_filter='DigiAssetTransaction.existingAssetTransactions'
./Google_Tests_run
```

### Useful filters

```bash
./Google_Tests_run --gtest_list_tests          # list everything
./Google_Tests_run --gtest_filter='DigiDollar*' # DigiDollar decoders and database round trips
./Google_Tests_run --gtest_filter='Database*'   # database layer
```

---

# Special Thanks

### Major Financial Support:

RevGenetics [Longevity Supplements](https://www.RevGenetics.com)
