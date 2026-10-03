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

## Unreleased — DigiByte Core 9.26.6 and Thaw Day

The script changes (installer pins, monitor) reach nodes through master. The Thaw Day guard in
the node itself arrives with the next release.

DigiByte Core v9.26.6 (released 2026-10-01) carries **Thaw Day**: new DigiDollar block rules
that activate at mainnet block **24,490,000** (around November 1, 2026). The release notes say
every full node must upgrade before that height, DigiDollar user or not, because older
software can disagree about valid blocks after it — the same failure shape as the 2026 Groestl
split. The rules cover mint price checks, vault accounting and redemption, not transaction
formats, so the node's DigiDollar decoder needs no change for it.

- **`setup-digiasset.ps1` 2.31.0 and `install-digibyte.ps1` 1.1.0** pin 9.26.6 for fresh
  installs (`-DigiByteVersion`). Existing nodes already move to it on their own: the
  maintenance task updates DigiByte to the latest release.
- **`monitor-node.ps1` 1.5.0** warns on DigiByte older than 9.26.6, with the number of blocks
  left before Thaw Day, and fails once the chain is past it. The peer-count line is now
  reported independently of the version line.
- **readme** manual-install link points at the 9.26.6 installer.

### The node will not index past Thaw Day against older DigiByte (binary — next release)

A flat minimum of 9.26.6 would stop every node whose DigiByte the maintenance task has not
updated yet, weeks before it matters. So the minimum depends on the height, which is the
actual risk (`THAW_DAY_HEIGHT` = 24,490,000, matching `consensus.nDDThawDayHeight` in DigiByte
Core v9.26.6 `src/kernel/chainparams.cpp`; `THAW_DAY_NODE_VERSION` = 92606):

- **At startup**, DigiByte older than 9.26.6 is a warning with the blocks left while the chain
  is below Thaw Day, and a refusal to start once it is at or past it. 9.26.5 stays the hard
  floor below that height.
- **While running**, the chain analyzer asks DigiByte for its version the first time it reaches
  a block at or past Thaw Day (once, then remembered). If it is older than 9.26.6 it stops
  before that block's database transaction with a clear cause, and the normal recovery path
  keeps retrying with back-off - so a node that started weeks earlier on 9.26.5 pauses at
  exactly the right block and resumes by itself once DigiByte is upgraded.

Left at 9.26.5 on purpose: statements about the published snapshot (built with 9.26.5) and the
DD-address spec citations.

---

## 0.3.3-win.141 — asset sends quote the fee they pay and stop splitting coins; DigiDollar DD… addresses; the pool can remove entries; the node comes back after a reboot

Two kinds of change here. The **asset wallet fixes, DD addresses and the pool's
`/permanent/remove`** are in the binaries and arrive with this release. Everything from
*At login, all three apps…* down is in `setup-digiasset.ps1` 2.30.0 / `make-snapshot.ps1` 2.5.0
/ `monitor-node.ps1` 1.4.0, which already reached nodes through the maintenance task's
self-update from master; they are listed here so the release notes are complete.

The wallet fixes and DD addresses come from mainnet testing of upstream DigiAsset Core
`asset_features` 92dae26 (PR #26) — a phone wallet round trip of asset 5381 — and apply to this
fork unchanged.

### Pool: POST /permanent/remove

`/permanent/add` is INSERT OR IGNORE and had no inverse, so a bad entry was permanent. A
publisher sent `ipfs://<cid>` strings for a while, leaving rows no node can pin (83 on
pool.digistamp.co's frontier page). `DigiAssetPoolServer.exe` now takes, with the same token as
add, either `{"token":"…","cids":"a,b"}` (remove those, reporting `requested` and `removed`) or
`{"token":"…","malformed":"true"}` (remove every row whose cid holds a character that is not a
letter or digit — no real CID does). Details and cautions in `pool/README.md`.

Verified on a local pool server with a scratch database: wrong token 403; both or neither mode
400; the sweep removed only the `ipfs://` row and left the valid CIDs; explicit removal of one
present and one absent CID reported requested 2 / removed 1. **Before sweeping the live pool**,
the publisher must send bare CIDs, or the next publish puts the rows back — the assets site's
`publishAssetToPool` now normalises every CID itself (Assets `697a19a`), so deploy that first.

### Asset send dry runs quote the fee the send actually pays

A `sendasset` dry run quoted `estimatedMinerFee` 0.00170192 DGB; the broadcast send then paid
0.0225 DGB, about 13x more. The dry run priced the fee itself with `estimatesmartfee` (conf
target 6, over a guessed size), while the send called `fundrawtransaction` with no fee option,
so the wallet funded at its own rate — 0.1 DGB/kB on the tester's node.

The dry run now funds the transaction exactly as the send does — same coin locking (asset
and unconfirmed coins can never pay the fee), same wallet fee rate — without signing or
broadcasting it, and reports the fee the wallet chose. Funding moved into one helper
(`fundOnce` in `AssetWallet.cpp`) used by both paths, so they cannot drift apart again. Applies
to the dry runs of `sendasset`, `burnasset`, `issueasset` and `reissueasset`.

The send itself is unchanged and still pays the wallet's rate: matching the quote to the wallet
keeps the operator's fee settings in charge, rather than overriding them with a lower rate. If
the wallet cannot fund the transaction at quote time (not enough confirmed DGB), the dry run
falls back to an estimate — now at the wallet's `paytxfee` when one is set.

### A small send no longer splits a large asset coin

For a 1-unit send the wallet split a 96-unit coin although three 1-unit coins of the same asset
were in the wallet; selection was largest-first only. It now prefers, among confirmed coins
holding only that asset: one holding exactly the amount (no asset change at all), else the
smallest single coin that covers it (keeps big coins whole), and only then gathers coins
largest-first as before. Coins carrying other assets too are never chosen as a single-coin
match, since they would drag those assets into the transaction as change.

### Already fixed here, still open upstream

- The flood of `nonstandard: <txid>` lines (63,760 in one sync) for `6a bf 01…` coinbase outputs:
  this fork classifies those as DigiDollar oracle commitments and prints only with
  `DGBCORE_DEBUG_SCRIPTS` set.

### DigiDollar addresses in their DD… form

The same testing found DigiDollar outputs reported only under their taproot `dgb1p…` address,
while DigiByte wallets show them as `DD…`. The node takes addresses from DigiByte Core's
`getrawtransaction`, which knows nothing of the DD form.

**The spec**, from DigiByte Core v9.26.5 `src/base58.cpp` (`CDigiDollarAddress`):

    DD address = Base58Check( version (2 bytes) || 32-byte taproot output key )
    version: mainnet 0x52 0x85 -> "DD…"   testnet 0xb1 0x29 -> "TD…"   regtest 0xa3 0xa4 -> "RD…"

The 32 bytes are the witness program of the P2TR output, the same bytes inside the `dgb1p…`
address, so the two forms convert losslessly. Only taproot outputs have a DD form. DigiByte
Core's DigiDollar wallet RPCs list DD balances under the DD form of each DD-holding output.

**In the node** (`DigiDollar::toDigiDollarAddress` / `fromDigiDollarAddress` /
`normalizeAddress`): a BIP350 bech32m codec plus Base58Check, network taken from the address
itself (`dgb` ↔ DD, `dgbt` ↔ TD, `dgbrt` ↔ RD).

- **Storage and indexing stay on the `dgb1p…` form** — what the chain and DigiByte Core use,
  so balances, history and existing callers are unaffected.
- **`getrawtransaction`** adds `ddAddress` to every output that carries DigiDollar, next to
  `address`.
- **`getaddressholdings` and `listaddresshistory`** accept a `DD…` address and look it up as
  its `dgb1p…` form.
- **DigiDollar events** (`digiDollarMint` / `digiDollarTransfer` / `digiDollarRedeem`) gain
  `ddAddresses`: the DD form of each address that received DigiDollar. `addresses` is
  unchanged.

Tests use vectors computed by a separate implementation (PowerShell / .NET SHA-256) whose
bech32m and bech32 encoders reproduce the BIP350 and BIP173 reference vectors, including a real
mainnet DigiDollar output (`dgb1pwrwn938…` ↔ `DD26bf7gt…`), all three network prefixes at both
ends of the key range, and rejection of non-taproot, mis-checksummed, whitespace-wrapped and
truncated input. The CLI now links `Base58.cpp` and `crypto/SHA256.cpp`, which `DigiDollar.cpp`
needs.

### At login, all three apps are started, watched and restarted

The wallet, IPFS Desktop and the node are desktop apps started by **logon** tasks. The node's
launcher only supervised the node: IPFS Desktop was started once and never again, and the
DigiByte wallet was left to its own logon task, so if either exited nothing brought it back.

- The launcher (`-Mode LaunchNode`) now brings the stack up in dependency order — DigiByte
  wallet, IPFS Desktop, then the node once both answer — and every ~30-60 s restarts whichever
  of the three is not running, for the whole session. `stop-node.ps1` is how to stop them.
- If DigiByte is running headless (the SYSTEM updater restarts it that way, since it cannot open
  a window in the user's session), the launcher swaps it back to the wallet window right away
  instead of at the next logon. That swap now stops the daemon via RPC `stop`; it used to
  hard-kill it, which risks a chainstate reindex.

### supervisor.pause — so a restart can't land in the middle of deliberate work

Restarting everything that exits needs an off switch for things that stop the stack on purpose,
and two of those were already racing the node-only supervisor:

- **make-snapshot.ps1** stops the node and archives `chain.db`; the supervisor relaunched the
  node about 20 s later, mid-archive, so the published `chain.db` could be taken from a live
  database. make-snapshot 2.5.0 holds the pause for its whole run.
- **The maintenance updater** stops the node/DigiByte to swap exes; the supervisor could
  relaunch the old exe onto the file being replaced, failing the update on a locked file. Both
  updates, and the installer's node swap, now hold the pause.

`C:\DigiAssetWindows\supervisor.pause` stops the launcher restarting anything; it resumes when
the file goes. A file older than 6 h is treated as left behind by a crashed run and ignored.

### Windows auto-login: recommended, checked, and reminded

Without auto-login, an unattended reboot (Windows Update) leaves everything down until someone
signs in. It stays the operator's choice — the installer never blocks on it.

- **Install offers it up front** (question 2 of 3) when `Winlogon\AutoAdminLogon` is not on for
  the installing user: ENTER downloads Sysinternals Autologon, refuses to run it unless it
  carries a valid Microsoft signature, opens it for the user to type their own password, and
  re-checks; `N` carries on. The password never passes through this script — Autologon stores
  it as an encrypted LSA secret. `-SkipAutologon` suppresses the offer.
- **The check catches the quiet failures:** auto-login on for a *different* account than the
  start-up tasks belong to, an `AutoLogonCount` that will switch it off after N logons, and
  Windows 11's "only allow Windows Hello sign-in", which blocks password auto-login.
- **Reminders until it is on:** the closing summary, every maintenance run's log, and
  monitor-node's *Auto-start* line.
- **The installer sets plugged-in sleep and hibernate to Never** — asleep, a node is as down as
  after a reboot. Battery settings are untouched.
- **Maintenance repairs start-up every run:** disabled start-up tasks are re-enabled, and missing
  logon tasks are recreated for the user now recorded in `state.json` at install.
- **`monitor-node.ps1` 1.4.0** adds *Auto-start* (tasks present and enabled, auto-login on and
  permanent) and *Sleep (plugged in)*.

### Existing nodes get the win.140 defaults

win.140's installer only wrote the tool RPC allow-list into configs it created or was re-run
over, and only installs dropped the companion tools. The maintenance task (SYSTEM, every 6h)
now does both on every run:

- **`rpcallow<method>=1`** for `version`, `syncstate`, `getnodestats`, `getipfscount` and
  `shutdown` is added to `config.cfg` when missing — per key, so an explicit `=0` stands.
  Done before the binary update, so a node restarted by that update comes up with it.
- **`monitor-node.ps1`, `stop-node.ps1`, `update-node.ps1`, `memwatch.ps1`** are refreshed
  from master when they differ; a download that does not parse never replaces a working copy.

And the auto-updater no longer hard-kills the node to swap its exe. It asks `cli shutdown`
first and waits up to 2 minutes, killing only as a last resort (logged). With SQLite in
`journal_mode=MEMORY`, the old kill could leave `chain.db` torn on any update.

---

## 0.3.3-win.140 — logs that say what went wrong, and health checks that catch it

Prompted by the 2026-09-26 pool outage. `DigiAssetPoolServer.exe` went down while Caddy in
front of it stayed up, so every `/permanent/*.json` request got a 502 with an empty body. The
nodes logged `PSP permanent page 23 returned non-JSON (len=0)` and an empty
`PSP keepalive returned UNEXPECTED response:` — which reads like a parsing bug, not "the pool
is down and nothing is being pinned anywhere".

### Pool (PSP) warnings name the HTTP status and what it means

`CurlHandler::get()`/`post()` return the body for any status, so callers never saw the 502.
Each thread now records its last status (`CurlHandler::lastHttpStatus()`), and the pool client
reports it with a plain-English reading and the first 160 characters of the reply:

- **502/503/504** — front end up, pool server behind it down; nothing new can be pinned.
- **401/403** refused, **404** not found, other **5xx** a server error.
- **200 with an empty body** — names the pool-box loopback setting (`psp server
  http://127.0.0.1:14028`), the known cause when it happens on the pool box itself.

A 5xx wrapped in JSON now counts as a failure; it used to be read as "page not ready yet".

Failures are tracked as an outage: the first says nothing new will be pinned, repeats carry
`(failure 4 in a row, for 32 min)`, and recovery logs one line — `PSP pool reachable again ...
after 4 failed attempt(s) over 32 min. Assets minted during the gap may need a pool
back-fill.` Keepalive gets the same treatment, with a recovery line noting the pool may not
have counted the node online for the gap.

### IPFS timeouts say whether the daemon is cut off or the content has no provider

`IPFS node did not answer "pin/add/..."` now appends the daemon's peer count with a reading
(0 = not connected to the network; under 10 = check port 4001; otherwise the content most
likely has no reachable provider) and the IPFS job queue depth. Only computed when the
rate-limited warning actually fires.

### Installer: the operator tools' RPC calls are allowed

With no `rpcallow` lines the node refuses every RPC method, and the installer wrote none. So
on an installer-built node `DigiAssetWindows-cli syncstate` and `shutdown` were refused — which
is how `make-snapshot.ps1` failed with "did not shut down cleanly within 60s". The installer
(`setup-digiasset.ps1` 2.27.0) now adds `rpcallow<method>=1` for
`version`, `syncstate`, `getnodestats`, `getipfscount` and `shutdown`, to new configs and to
existing ones that lack them. An explicit `=0` is left alone. RPC stays loopback-only and
authenticated. **Existing nodes need the installer re-run (or those lines added) and a node
restart.**

### monitor-node.ps1 1.3.0

New checks: DigiByte height **and block hash** against digiexplorer.info / chainz (a node on a
stale fork shows as FAIL even while reporting "synced 100%"); DigiByte version (older than
9.26.4 stalls on the post-split Groestl blocks) and peer count; the DigiAsset node's height
behind DigiByte and its state, with a **stuck** flag in `-Watch` mode when the height stops
moving for 10 minutes; IPFS peers, Desktop version, bitswap blocks served; the IPFS job queue;
the pool's permanent list itself (a 502 there is the outage above); and a 10-CID sample of that
list checked with `pin/ls` to show whether pinning is keeping up.

### Snapshot scripts

`make-snapshot.ps1` 2.4.1 / `publish-snapshot.ps1` 1.5.0: wait for DigiByte and the node to
answer RPC after restarts (`-StartWaitSec`, default 600) and up to `-StopWaitSec` (default 600,
was 60) for a clean node exit; keep the CLI's output and put it in the error when shutdown is
refused. `syncstate` has no `height` field (it returns `count`/`sync`), so the chain.db height
never parsed and was always published as 0 — it now comes from `getnodestats.syncHeight`.

### Also in this release — pool federation: tooling, and the reverse-proxy bug blocking it

No binary change. Investigating whether two pools can find each other on-chain turned up a
deployment bug that made it impossible, plus a script to prove the path end to end.

#### /peer/* was 404 on the live pool — federation could never have worked

Pool-to-pool discovery is fully implemented: a pool announces itself in a `DGSP1<url>`
OP_RETURN (weekly-gated), scans the chain for other pools, keeps discovered ones in an
untrusted display-only `directory[]`, and merges explicitly-configured `poolpeers` into
`peers[]`. `/pool/stats.json` publishes both.

None of it could run. Every `/peer/*` request to the live pool returned **404**, while
`/pool/stats.json`, `/nodes.json`, `/map.json` and `/bad.json` all answered 200. The routes
are registered unconditionally in `PoolServer.cpp`, and the repo's Caddyfile template lists
`/peer/*` in its `@api` matcher — but the **resolved** Caddyfile that Caddy actually runs
predated that line.

`update-pool.ps1` downloads the Caddyfile *template* into the deploy folder. Caddy does not
read that file. `setup-caddy.ps1` resolves it — substituting domain, ports, site root — into
`C:\DigiStampPool\Caddyfile`, and nothing re-runs that step. So a template that gains a route
never reaches the running proxy, silently, forever.

`update-pool.ps1` now compares the routes in the template against the live resolved file and
reports any that are missing, with the command to fix it. It deliberately does **not**
regenerate automatically: that restarts the proxy, which a binary update should not do behind
the operator's back.

**Existing pool boxes need `setup-caddy.ps1` re-run once** before federation will work.

#### pool/deploy/verify-federation.ps1

`verify-peers.ps1` already checks that two pools which know about each other can talk. This
checks the layer beneath: that they can find each other with **no shared config**, which is
what "nobody depends on one operator" actually rests on.

Six phases — preflight (both pools up, `poolpublicurl` set and not loopback, `DGSP1`+url fits
an OP_RETURN's 78 bytes, wallet funded), optional forced announcement, on-chain verification,
confirmation wait, discovery polling, and a trust-boundary report.

Phase 3 is the point of it. A pool reporting "announced" proves nothing; the script pulls the
transaction back out of DigiByte Core, finds the OP_RETURN, decodes `DGSP1` and asserts the URL
matches the pool's own `poolpublicurl` — catching an announcement that encoded wrongly or
points somewhere unreachable.

Read-only unless `-Announce` is passed, which spends one small fee. Works with a single pool
(phases 1-4) so the announcement side can be proven before a second box exists.

The decoder has unit tests covering a real announcement, a script with no marker, an empty
script, `DGSP1` with nothing after it, a payload interrupted by non-printable bytes, an
odd-length trailing nibble, and uppercase hex.

Reading `digibyte.conf` now degrades instead of throwing: the installer ACLs it to
SYSTEM + Administrators, so a non-elevated run got an unhandled `UnauthorizedAccessException`.
It now says to re-run as Administrator and skips only the on-chain phases.

#### Known gap: nodes are not part of federation

Worth recording, because it is the difference between what exists and the goal. Pools discover
each other; **nodes do not**. A node's pool is one config line (`psp<N>server`), there is no
peer or directory awareness on the node side and no failover, so if a pool disappears its nodes
are stranded until a human edits `config.cfg`.

Closing that means a node reading `network.peers[]`/`directory[]` from the stats it already
fetches, caching them locally so they survive the pool being down, and failing over on the
keepalive path. The pool side needs no changes — it already publishes everything required.

The trust rule matters more than the code: `autoremovebad` defaults to **true**, so a pool's
`bad.json` can make nodes **unpin** content. A pool a node failed over to must be pin-only
until an operator promotes it, or chain discovery becomes a way to delete assets across the
network for the price of one transaction.

---

## 0.3.3-win.139 — a normal reorg no longer reports itself as a sync error

Found by reading the sync path rather than by a failing test, while auditing before deployment.

`mainFunction`'s recovery preamble was gated on `_hasRunOnce` — "has mainFunction been called
before" — which is true forever after the first pass. But `phaseSync()` also **returns cleanly**
when it detects a fork (`_state = REWINDING`), which is a legitimate reorg and not an error. So
every normal reorg re-entered the recovery path and logged:

```
Auto-recovered from a sync error at block <stale> (attempt 1). Cause: unknown error
```

It also rolled back a block that had never been partially written, and incremented the error
count toward the give-up threshold — all on a completely healthy node. The block number was
whatever the last real error had been, and the cause was literally "unknown error", because
`_lastError` had been cleared by the successful pass.

The gate is now `_lastPassFailed`, set only in the two catch blocks and cleared on a pass that
does not throw. The recovery preamble runs for an actual failure and nothing else.

This is pre-existing — the same structure was there before this run of work — but it directly
undermines the thing the last several releases were about. win.130 through win.132 were spent
making the analyzer's log trustworthy after a node sat dead for 4.6 hours while reporting itself
healthy; a log that cries "sync error" on every reorg is the same disease.

### Scripts and docs brought in line with the win.137 bootstrap removal

`bootstrapchainstate` no longer exists in the code, but four places still referenced it — and
one of them was **writing it into every config.cfg the installer generated**:

- `setup-digiasset.ps1` — emitted `bootstrapchainstate=1` and documented it in the generated
  config header. Both removed; the key would have sat there inert and misleading on every new
  install.
- `node/test-asset-lifecycle.ps1`, `node/test-pr26-smoke.ps1` — set `bootstrapchainstate=0`.
- `ARCHITECTURE.md` — listed it among the analyzer's config keys; now names `trackdigidollar`.
- `readme.md` — a whole "Generating a Bootstrap Image" section describing `--bootgen`, the
  vacuum-to-single-file behaviour, and how to publish a new CID. Replaced with a section on how
  this fork actually fast-syncs (the R2 snapshot, and why `make-snapshot.ps1` ships `chain.db`
  **with** its `-wal`/`-shm` after a clean stop), plus a short note recording what was removed
  and why.

### Known limitation worth checking on first deploy

The DigiDollar marker fix adopted in win.137 records `ddSyncHeight` when the sync path passes
**exactly through** the activation block (23,869,440). A node restored from the fast-sync
snapshot starts *above* that height, so it never passes through it — for those nodes the marker
has to already be present in the shipped `chain.db`, which it is only if the machine that built
the snapshot had itself run the backfill.

Nothing here can verify the published snapshot's contents. On the first restore, watch for
`DigiDollar indexing has not been run. Rewinding from ... to 23869439`. If it appears, the
snapshot needs rebuilding from a node whose marker is set; the node is not broken either way,
it just replays ~212,000 blocks once.

103/103 unit tests pass. CFG and zero absolute paths hold on all three binaries.

---

## 0.3.3-win.138 — the documented build works from a clean checkout again

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
