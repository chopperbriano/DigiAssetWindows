# DigiAsset for Windows

> **This is a Windows port of [DigiAsset Core](https://github.com/DigiAsset-Core/DigiAsset_Core) originally created by [mctrivia](https://github.com/mctrivia).** All core logic, chain analysis, RPC methods, and DigiAsset protocol implementation are their work. This repository only adds Windows (MSVC) build support, platform-specific stubs, and a console dashboard UI.

**One-liners:** [Install a node](#quick-install-recommended) ·
[Update & maintain a node](#update--maintain-an-existing-node) ·
[DigiByte wallet only](#install-a-digibyte-core-wallet-seeded-standalone) ·
[Seed an existing wallet](#already-have-digibyte-core-installed) ·
[Snapshot box](#snapshot-box-refresh-the-snapshot-scripts)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Quick install (recommended)

Host DigiAsset content and earn DGB. You do not need to clone or build anything.
On the Windows PC that will run the node, open **PowerShell as Administrator**
(click Start, type `PowerShell`, right-click **Windows PowerShell**, choose *Run as
administrator*) and paste this single line:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-digiasset.ps1 -OutFile "$env:TEMP\setup-digiasset.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-digiasset.ps1"
```

- It asks for your **DGB payout address**. Press ENTER and it creates one in the
  local wallet.
- It offers **Windows auto-login**, so the node comes back by itself after a
  reboot. Optional.
- **Forward TCP 4001** on your router to this PC. That is how the pool verifies
  you and pays you.
- **Walk away.** It installs DigiByte Core, IPFS Desktop and the node, fast-syncs
  from the published snapshot, and keeps itself updated.

Your node joins the **DigiStamp pool** (`pool.digistamp.co`) on its own. You don't
run a pool. Full walkthrough: **[NODE-SETUP.md](NODE-SETUP.md)**.

> **Thaw Day.** DigiByte Core **9.26.6 or newer** is required before mainnet block
> **24,490,000** (about Nov 1 2026). The installer (it pins 9.26.7) and the
> maintenance task take care of it. On older DigiByte the node refuses to index
> past that block.

## Update & maintain an existing node

A node already keeps itself current: its maintenance task checks every 6 hours and at
boot, updates DigiByte Core and the DigiAsset binaries, and refreshes the helper
scripts. Use these when you want something **now**. Every line downloads the
**latest** script from this repo and runs it; paste into an **Administrator
PowerShell** (they self-elevate). Each one is safe to re-run.

**Update now** — the latest release's `DigiAssetWindows.exe` + CLI (and
`DigiAssetPoolServer.exe` on a box that has it), SHA256-checked, swapped in after a
clean node shutdown, plus the web console. This is the updater to use:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/node/update-binaries.ps1 -OutFile "$env:TEMP\update-binaries.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\update-binaries.ps1"
```

The installer also leaves a copy at `C:\DigiAssetWindows\update-binaries.ps1`.

**Pool box** — the same, making sure the pool server is updated too:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/node/update-binaries.ps1 -OutFile "$env:TEMP\update-binaries.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\update-binaries.ps1" -IncludePool
```

**Repair or update everything** — DigiByte Core (to the pinned release, currently
**9.26.7**; it never downgrades a newer one), IPFS Desktop, config defaults and
start-up tasks — by re-running the installer. It keeps your config, payout address
and wallet, backs the wallet up first, and stops DigiByte cleanly before replacing it:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-digiasset.ps1 -OutFile "$env:TEMP\setup-digiasset.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-digiasset.ps1"
```

**Health check** — DigiByte version and whether it agrees with the network, DigiAsset
sync, IPFS, the pool, port 4001, auto-start. Saves the latest copy next to the node,
then runs it (add `-Watch` to keep it refreshing):

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/node/monitor-node.ps1 -OutFile C:\DigiAssetWindows\monitor-node.ps1 -UseBasicParsing; powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\monitor-node.ps1
```

**Refresh all the node helper scripts** into `C:\DigiAssetWindows` (monitor, stop,
both updaters, memory watch) without running anything:

```powershell
$b='https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/node'; 'monitor-node.ps1','stop-node.ps1','update-node.ps1','update-binaries.ps1','memwatch.ps1' | % { iwr "$b/$_" -OutFile "C:\DigiAssetWindows\$_" -UseBasicParsing }
```

`config.cfg` and `digibyte.conf` are locked to Administrators. To edit one, open
Notepad with **Run as administrator**, then open the file.

### Snapshot box: refresh the snapshot scripts

For the box that publishes the fast-sync snapshot. Downloads the latest snapshot
scripts into `C:\snapshots`; then run `C:\snapshots\publish-snapshot.ps1` as usual
(details in [snapshots/README.md](snapshots/README.md)). Bring DigiByte and the node
up to date on that box first, so the snapshot is built by current software:

```powershell
New-Item -ItemType Directory -Force C:\snapshots | Out-Null; $b='https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/snapshots'; 'make-snapshot.ps1','publish-snapshot.ps1','seed-digibyte.ps1','setup-cloudflare-snapshots.ps1','snapshot-digibyte-datadir.ps1' | % { iwr "$b/$_" -OutFile "C:\snapshots\$_" -UseBasicParsing }
```

## Install a DigiByte Core wallet, seeded (standalone)

Want **just DigiByte Core** — no IPFS, no DigiAsset node, no pool? This installs
DigiByte Core, writes `digibyte.conf`, seeds the blockchain from the snapshot,
opens the firewall, starts the wallet, **creates a wallet, and offers to encrypt
it**. In an **Administrator PowerShell**:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/install-digibyte.ps1 -OutFile "$env:TEMP\install-digibyte.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\install-digibyte.ps1"
```

Re-running is safe: an existing `digibyte.conf` is topped up and an existing
blockchain is left alone. Switches (`-DataDir`, `-Headless`, `-Lean`, `-SkipSeed`,
`-Force`) are listed in [CHEATSHEET.md](CHEATSHEET.md#0-one-liners-no-repo-checkout-needed).
**Back up `wallet.dat` off the machine**; there is no recovery if the disk dies or
you lose the passphrase.

### Already have DigiByte Core installed?

Then you only need the **seeding** half. This downloads, verifies and extracts the
snapshot into an existing data directory. It prompts for the folder and
sanity-checks it first. Start DigiByte Core once and close it, then in an
**Administrator PowerShell**:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/snapshots/seed-digibyte.ps1 -OutFile "$env:TEMP\seed-digibyte.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\seed-digibyte.ps1"
```

Both scripts fetch the newest published snapshot (~37 GB download, needs ~90 GB
free), check the space first, SHA256-verify every archive, and fall back to a normal
sync on any failure.

> Which one do I want? **Nothing installed yet → `install-digibyte.ps1`.**
> **DigiByte already installed → `seed-digibyte.ps1`.** **Want the full DigiAsset
> node → `setup-digiasset.ps1`**, which already does all of this for you.

## How It Works (Architecture)

Three programs work together: **DigiByte Core** (the blockchain), **IPFS Desktop**
(file storage) and **DigiAsset for Windows** (`DigiAssetWindows.exe`, the node). The
node reads the chain from DigiByte Core and pins DigiAsset files on IPFS. Its web
console is at http://localhost:8090 (keep it private, never forward it). The
components, data flow and ports are in **[ARCHITECTURE.md](ARCHITECTURE.md)**.

**Running a pool** (accepting nodes and paying hosts) is a separate job that most
people never need. See **[POOL-SETUP.md](POOL-SETUP.md)**.

## Docs

- **[NODE-SETUP.md](NODE-SETUP.md)** — run a node and get paid: install, ports, monitoring, updating.
- **[CHEATSHEET.md](CHEATSHEET.md)** — one page, every script, by role.
- **[POOL-SETUP.md](POOL-SETUP.md)** — run a Permanent Storage Pool.
- **[BUILDING.md](BUILDING.md)** — build from source (developers only).
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — how the pieces fit together.
- **[CHANGELOG.md](CHANGELOG.md)** — release history.
- **[Releases](https://github.com/chopperbriano/DigiAssetWindows/releases)** — the `.exe`s and scripts for each build.

Reference: [pool/README.md](pool/README.md) (every `pool.cfg` key, verification, payouts) ·
[pool/deploy/README.md](pool/deploy/README.md) (pool deploy toolkit) ·
[snapshots/README.md](snapshots/README.md) (fast-sync snapshots) ·
[docs/asset-rules-and-burns.md](docs/asset-rules-and-burns.md) (when assets are destroyed).

## Credits

This project is a Windows port of **[DigiAsset Core](https://github.com/DigiAsset-Core/DigiAsset_Core)** by **[mctrivia](https://github.com/mctrivia)** and contributors. The core DigiAsset protocol implementation, chain analyzer, RPC interface, database schema, and all blockchain logic are entirely their work.

Matthew Cornelisse (mctrivia) also created the original DigiAsset Permanent Storage
Pool and its protocol. This fork's pool server builds on that design so the
community can run its own paying pools, like DigiStamp.

This fork adds only:
- Windows/MSVC build system and platform stubs (WinHTTP, OpenSSL stubs, vendored SQLite3)
- Console dashboard UI (VT100-based TUI)
- Embedded web server (no separate exe)
- Sync performance optimizations (prefetch pipeline, UTXO caching)
