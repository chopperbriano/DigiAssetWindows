# Run your own DigiStamp pool

This is the **pool operator** guide: how to stand up and run a Permanent Storage
Pool that other people point their nodes at, and (optionally) pays them DGB for
hosting DigiAsset content.

> **You almost certainly don't need this.** The **DigiStamp pool**
> (`pool.digistamp.co`) is already run for you — just install a node with
> **[NODE-SETUP.md](NODE-SETUP.md)** and it joins automatically. This doc is only
> for the (rare) case of running your *own* separate pool server.

A pool is one extra program — `DigiAssetPoolServer.exe` — that runs alongside a
normal node stack. It keeps the canonical asset list, accepts node
registrations, verifies that registered nodes really host the content, and (if
you enable it) pays them from your DGB wallet.

For the full architecture and every config key, see **[pool/README.md](pool/README.md)**.
For the HTTPS/landing-page details, see **[pool/deploy/README.md](pool/deploy/README.md)**.
This page is the start-to-finish happy path.

---

## What you need

- A **Windows** PC/VM that stays on (the pool server is Windows-only for now).
- A **domain name** you control (e.g. `pool.example.com`) with an **A record**
  pointing at the server's public IP — so nodes can reach `https://pool.example.com`.
- Inbound **TCP 80 + 443** reachable from the internet (for HTTPS via Caddy).
- The node stack (DigiByte Core + IPFS) on the same box — step 1 sets that up.
- A **DGB wallet** if you intend to pay hosts (you can run registration-only first).

---

## 1. Install the node stack (DigiByte + IPFS + node)

The pool server verifies nodes over IPFS and reads the chain + sends payouts over
DigiByte Core's RPC, so it needs both running locally. The easiest way to get
them (plus a node of your own) is the standard one-line installer. In an
**Administrator PowerShell**:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-digiasset.ps1 -OutFile "$env:TEMP\setup-digiasset.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-digiasset.ps1"
```

That installs the **DigiByte wallet**, **IPFS Desktop**, and a **node** as GUI apps
that open when you log in. For an always-on pool box, set the machine to auto-login
with **[Sysinternals Autologon](https://learn.microsoft.com/sysinternals/downloads/autologon)**
(see [NODE-SETUP.md](NODE-SETUP.md#auto-start--running-it-unattended)). Let DigiByte
finish syncing before you enable payouts.

## 2. Get `DigiAssetPoolServer.exe`

Download `DigiAssetPoolServer.exe` from the
[Releases](https://github.com/chopperbriano/DigiAssetWindows/releases) page and
put it in `C:\DigiAssetWindows` (next to the node), **or** build it from source
(`cmake --build build --config Release --target DigiAssetPoolServer`).

## 3. Write `pool.cfg`

Create `C:\DigiAssetWindows\pool.cfg`. Reuse the **same** `rpcuser`/`rpcpassword` that
the installer put in `C:\DigiByte\digibyte.conf` so the pool can reach
DigiByte Core. Minimal starting config:

```ini
poolport=14028
pooldbpath=pool.db
ipfspath=http://localhost:5001/api/v0/

# DigiByte Core RPC (copy from C:\DigiByte\digibyte.conf)
rpcuser=digiasset
rpcpassword=PASTE_THE_SAME_PASSWORD_HERE
rpcport=14022

# Public donation address shown on your pool page. GENERATE IT FROM THE POOL
# WALLET so donations land in the same wallet payouts spend from (one pot):
#   digibyte-cli -datadir=C:\DigiByte -conf=C:\DigiByte\digibyte.conf getnewaddress "treasury"
# (setup-pool.ps1 does this for you.) Its balance is read from a public explorer.
pooldonationaddress=A_GETNEWADDRESS_FROM_YOUR_POOL_WALLET
pooladdrapiprefix=https://digiexplorer.info/api/address/
poolexplorertxprefix=https://digiexplorer.info/tx/

# Payouts OFF until you have funded a wallet and run a smoke test (see step 6).
poolpayouts=0
poolpayoutpercent=10
poolpayoutperiodhours=24
```

Every key (and the payout options) is documented in
**[pool/README.md](pool/README.md#pool-operator-setup)**.

## 4. Start the pool

From an **Administrator PowerShell**, use the helper in `pool/deploy/`:

```powershell
powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1
```

It waits for DigiByte Core's RPC, checks IPFS is up, launches
`DigiAssetWindows.exe` + `DigiAssetPoolServer.exe`, and makes sure the website
task is running. Safe to re-run. The pool exe shows a live dashboard —
`[P]` previews payouts, `[E]` executes them (both covered in step 6).

## 5. Publish it over HTTPS

Nodes connect to `https://<your-domain>`, but the pool exe only speaks plain HTTP
on 14028. Put Caddy in front to terminate TLS and serve the landing page. From
`pool/deploy/`, in an **Administrator PowerShell**:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup-caddy.ps1 -Domain pool.example.com
```

Caddy auto-obtains a certificate, serves `site/index.html` (donation QR + treasury
balances + payout ledger + the **world map of nodes**), and reverse-proxies the
pool API routes to `127.0.0.1:14028`. DigiByte's RPC port is never exposed. Give
it 10-30 s on first run for the certificate. Details:
**[pool/deploy/README.md](pool/deploy/README.md)**.

## 6. Fund it and turn on payouts

The key thing to understand: **`[E]` spends from the pool box's DigiByte Core
wallet** (via `getbalance`), and the budget is `poolpayoutpercent`% of *that
wallet's* balance. It does **not** spend from the `pooldonationaddress` label.

**Recommended (one pot):** because `pooldonationaddress` was generated *by the
pool wallet* (step 3 / `getnewaddress`), donations land straight in the wallet
`[E]` spends — nothing to sweep. Fund it by pointing donors at that address.

> **Advanced (two pots, cold treasury):** if you'd rather keep donations in a
> separate cold wallet, set `pooldonationaddress` to that cold address instead —
> but then it's a *display-only* label and you must **manually sweep DGB from the
> cold treasury into the pool box's wallet** to fund payouts. Only do this if you
> understand the trade-off; the one-pot default is simpler and can't be mixed up.

Then:

1. Make sure the pool wallet has DGB and DigiByte Core is **synced + unlocked**.
2. Set `poolpayouts=1` in `pool.cfg` and restart the pool exe.
3. Press `[P]` to **preview** (read-only): eligible nodes, budget, per-node amount.
4. Press `[E]` to **execute** — it re-validates and asks Y/N before sending.

A node is eligible only if it was verified (reachable) in the last 24 h, has
< 3 consecutive verification failures, and was seen in the last 7 days.

## 7. Keep it running

- **Verify the whole stack** — run `pool/deploy/verify-pool-stack.ps1` any time you
  touch a config. It cross-checks that `digibyte.conf`, `config.cfg`, and `pool.cfg`
  agree on the RPC handshake, confirms DigiByte Core is reachable + synced, and
  proves **every full-node index (txindex, coinstatsindex, block filters, DigiDollar)
  is enabled and current** — so the node fully serves old and new DigiByte features.
  Add `-Fix` (elevated) to append any missing index/feature setting to
  `digibyte.conf`, then restart Core so it builds.
- **Auto-updates** — because `DigiAssetPoolServer.exe` lives in `C:\DigiAssetWindows`, the
  node's maintenance task keeps it **in sync with the node**: when a new release
  ships, it stops the pool, swaps the exe, and restarts it (the pool + node are
  released together). You can also update on demand with
  `update-binaries.ps1 -IncludePool`.
- **Backups** — `pool/deploy/backup-digistamp.ps1` snapshots `pool.db` (the
  ledger + registrations), configs, and the DigiByte wallet, rotating the newest
  few. Best run while stopped. See [pool/deploy/README.md](pool/deploy/README.md).
- **Auto-start on boot** — register a logon task that runs
  `backup-digistamp.ps1` then `start-digistamp.ps1` (snippet in the deploy README).
- **Advertise it** — there is no in-app pool discovery. Nodes only find you if
  you share the URL. A copy-paste "join my pool" snippet is in
  [pool/README.md](pool/README.md#advertising-your-pool-join-my-pool-snippet).

---

## Router / firewall

Your pool server hosts the same way a node does, plus the website:

| Port | Where | For |
|---|---|---|
| **80 + 443** TCP | router → this box | HTTPS website + Let's Encrypt (Caddy) |
| **4001** TCP+UDP | router → this box | IPFS hosting + node verification |
| **12024** TCP | router → this box | DigiByte P2P (recommended) |
| 5001 / 14022 / 8090 / 14028 | **local only** | never forward — IPFS API, RPC, web UI, pool HTTP (Caddy proxies it) |

The node installer already opened 4001/12024 on the local Windows firewall;
`setup-caddy.ps1` opens 80/443. You still forward them on your **router**.
