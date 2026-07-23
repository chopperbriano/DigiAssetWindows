# DigiAssetPoolServer

An optional companion executable (`DigiAssetPoolServer.exe`) that lets an
operator run their **own** Permanent Storage Pool server for the Windows fork.
It builds on the excellent pool protocol mctrivia designed for
`ipfs.digiassetx.com`, re-implementing that wire protocol so the community can
run independent pools: serving the permanent-asset list, accepting node
registrations, verifying that registered nodes actually host the content, and —
if the operator opts in — paying verified nodes in DGB. Full credit to mctrivia
for the original design this stands on.

There are two roles:

- **Pool operator** — runs `DigiAssetPoolServer.exe`, owns the DGB wallet, and
  decides whether/when to pay.
- **Node operator** — runs `DigiAssetWindows` + an IPFS (kubo) node, points it at a
  pool via `psp2server=`, and registers a payout address to earn DGB for hosting.

> New here? Read the [architecture overview](../readme.md#how-it-works-architecture)
> in the top-level readme first — it explains how DigiByte Core, IPFS, and
> DigiAssetWindows fit together. This file covers only the pool server on top.

### How the pieces talk

```
  Node operators (many)                        Pool operator (one)
  ─────────────────────                        ───────────────────
  DigiAssetWindows.exe                            DigiAssetPoolServer.exe
    │  register peerId + payout addr  ──POST /list──►  records node in pool.db
    │  keepalive                      ──POST /keepalive►
    │  fetch asset list              ◄─GET /permanent◄  serves canonical CIDs
  IPFS (kubo)                                    │
    ▲   pins the pool's CIDs                     │ verify: dial peer, else
    └────────────────────────────────────────────┘ findprovs + bitswap
                                                 │
                                                 │ [E] pay verified nodes
                                                 ▼
                              DigiByte Core wallet ──sendtoaddress──► node payout addrs
```

The pool never holds anyone's coins in escrow: it reads its own DigiByte Core
wallet and sends DGB directly to each verified node's registered address when the
operator presses `[E]`.

---

## ⚠️ Node operators: forward IPFS port 4001 (strongly recommended)

**This is the most common reason a node is slow to start earning — forward the
port and you remove all doubt.**

Before paying anyone, the pool **verifies** each registered node two ways:

1. **Direct dial** (`swarm/connect`) — the strongest proof. Works when your node
   is reachable.
2. **Provider-record fallback** — if the dial fails, the pool checks the DHT
   (`findprovs`) to see whether your peerId is announced as a provider of the
   permanent-list content you should be pinning, and confirms that content is
   actually fetchable. This is **NAT-tolerant**, so a node behind a home router
   can still verify and get paid.

A node passes if **either** check succeeds, so port-forwarding is **not strictly
required** — but it is strongly recommended, because the fallback depends on the
DHT having propagated your provider records (which can lag on home routers) and
is a weaker signal overall. **Forwarding the port makes you verify immediately
and reliably.**

To be reliably verifiable:

1. **Forward TCP port 4001** (IPFS swarm port) on your router to the machine
   running kubo. If you changed kubo's `Addresses.Swarm` port, forward that one.
2. **Announce a reachable address** — set `Addresses.Announce` in your kubo
   config to your public `/ip4/<WAN-IP>/tcp/4001` (the DigiAssetWindows dashboard's
   `[F]` key will do this for you when it detects port-open-but-not-announced).

If you are behind CGNAT and cannot open a port, you may still earn via the
provider-record fallback, but verification will be less reliable — expect delays
and occasional missed periods until your provider records propagate.

**Check your reachability:**

- In the DigiAssetWindows dashboard, press `[P]` (runs an `ifconfig.co/port/4001`
  reachability check) — it should report the port as open.
- From another machine: `ipfs swarm connect /ip4/<your-WAN-IP>/tcp/4001/p2p/<your-peerId>`
  should succeed.

> Pool operators: please still advertise the port-4001 recommendation wherever
> you promote your pool (Discord/Twitter/website). Forwarded nodes verify
> instantly; NAT'd nodes work but are less reliable.

---

## Pool operator setup

Build produces `build/pool/Release/DigiAssetPoolServer.exe`. It reads `pool.cfg`
from the working directory. Keys:

| Key | Default | Meaning |
|---|---|---|
| `poolport` | `14028` | TCP port the pool server listens on |
| `pooldbpath` | `pool.db` | sqlite file for pool state |
| `pooladmintoken` | *(unset)* | Shared secret gating `POST /permanent/add`. Unset = the ingestion endpoint is **disabled** (403). Set it (and give the same value to your publisher, e.g. the marketplace) to let a trusted service push freshly-minted asset CIDs onto the permanent list. |
| `ipfspath` | `http://localhost:5001/api/v0/` | kubo HTTP API base the verifier uses for dial-back, `findprovs`, and `cat`. The pool operator's own IPFS node must be running for verification (including the NAT fallback) to work. |
| `poolpayouts` | `0` | **Foot-gun.** `1` enables payouts: the pool advertises `payoutsEnabled:true` and the `[E]` key can send DGB. Leave `0` until you've funded a wallet and run a smoke test. |
| `poolpayoutpercent` | *(unset)* | **Balance-derived budget (recommended).** Percent of the wallet's *spendable balance* to pay out per period (e.g. `10` = 10%), split in proportion to each node's coverage x reliability weight (see FAIRNESS.md). Because it scales with the balance it can never overspend an empty wallet — ideal for a donation-funded pool. Takes precedence over `poolspendperperiod` when set. |
| `poolspendperperiod` | *(unset)* | Fixed DGB budget per period, split in proportion to each node's coverage x reliability weight (see FAIRNESS.md). Used only when `poolpayoutpercent` is unset. |
| `poolpayoutperiodhours` | `24` | Minimum hours between payouts. `[E]` refuses a second payout until this elapses, so a double-press can't double-pay. Set `0` to disable the guard (not recommended). |
| `poolwalletpassphrase` | *(unset)* | Passphrase for an **encrypted** pool wallet. When set, `[E]` unlocks the wallet with `walletpassphrase` just for the payout batch and re-locks it immediately after, so encrypted-wallet payouts work unattended. Leave blank for an unencrypted wallet. Keep `pool.cfg` readable only by the operator. |
| `rpcuser` | *(unset)* | DigiByte Core RPC username — required for wallet `sendtoaddress`, `getbalance`, and the stats page. Copy from your DigiByte Core config. |
| `rpcpassword` | *(unset)* | DigiByte Core RPC password. |
| `rpcport` | `14022` | DigiByte Core RPC port. |
| `pooldonationaddress` | *(unset)* | DGB address published on the pool web page for donations/fees. May live in **any** wallet (e.g. a separate treasury/cold wallet). Its balance + received total are read from a public explorer (see `pooladdrapiprefix`), so it does **not** need to be in the pool node's wallet. |
| `pooladdrapiprefix` | `https://digiexplorer.info/api/address/` | Esplora-style address API used to read the treasury address's balance + received total for the stats page. Full URL is `<prefix><address>`. |
| `poolexplorertxprefix` | `https://digiexplorer.info/tx/` | Base URL the web ledger links payout txids to. |

The pool talks to a **local** DigiByte Core over RPC at `127.0.0.1:<rpcport>`
using these credentials, so DigiByte Core must be running with `server=1` and a
matching `rpcuser`/`rpcpassword`, and its wallet must be funded and **unlocked**.

### Payout flow

- `[P]` — **preview** (read-only): shows the eligible verified nodes, the budget
  (balance-derived or fixed), the per-node amount, and totals paid to date.
- `[E]` — **execute**: validates config, computes the budget, then asks `Y/N` to
  confirm before sending. Refuses if `poolpayouts=0`, no verified nodes are
  eligible, `rpcuser` is missing, the wallet balance can't be read, the budget is
  zero, or the last payout was within `poolpayoutperiodhours`.

### Public donation / treasury page (`GET /pool/stats.json`)

The pool exe serves a read-only JSON endpoint with `{donationAddress,
receivedTotal, treasuryBalance, available, paidToHosts, verifiedNodes,
totalNodes, nodes[...], recentPayouts[...]}`. The Caddy landing page
([deploy/](deploy/README.md)) renders it as a live donation QR + treasury
balances + a public payout ledger + a **world map of nodes**, cached ~30s.
DigiByte Core's RPC port is **never** exposed — only this computed JSON is,
through Caddy.

The `nodes` array is `[{lat, lon, city, country}]`, one entry per node seen in
the last 7 days. The pool geolocates each node's IP (from its multiaddr) via
`ip-api.com`, **server-side and per-IP cached** so the public endpoint never
hammers the geo service. The landing page plots them with Leaflet + marker
clustering (dark Carto tiles). No IPs or peerIds are exposed to the browser —
only coarse lat/lon + city/country.

**Two pots (important):**

- **Treasury** = the published `pooldonationaddress`, in whatever (possibly
  external/cold) wallet you like. `receivedTotal` and `treasuryBalance` come from
  the public explorer API, so this address does not need to be on the pool node.
  This is where donations *and* product fees (ProofLink/SignalFire/tickets) land.
- **Pool wallet** = the DigiByte Core wallet on the pool server that `[E]`
  actually spends from. `available` is its `getbalance`, and balance-derived
  payouts (`poolpayoutpercent`) draw from it.

Because these are separate wallets, **you must periodically sweep DGB from the
treasury into the pool wallet** to fund payouts — the pool can only pay from its
own local balance. The page shows the whole funnel: `Donated in` → `In treasury`
→ (you sweep) → `Paid to hosts`.

A node is **eligible for payout** only if it was verified (reachable) within the
last 24h, has fewer than 3 consecutive verification failures, was seen in the
last 7 days, AND is provably hosting (`coverageScore > 0`). Each eligible node's
share is then **weighted by its coverage x reliability** — see **[FAIRNESS.md](FAIRNESS.md)**.

---

## Marketplace / operator ingestion — `POST /permanent/add`

So freshly-minted assets propagate to wallets, a trusted publisher (e.g. the
DigiStamp marketplace) can push an asset's CIDs onto the permanent list. The
pool nodes then pin them, announce them to the IPFS DHT, and any indexer /
wallet resolving those CIDs can fetch them.

- **Auth:** set `pooladmintoken` in `pool.cfg` and send the same value in the
  request. Unset token = endpoint disabled (403).
- **Request:** `POST /permanent/add`, JSON body:
  ```json
  { "token": "<pooladmintoken>",
    "assetId": "La6Xoi...",
    "txHash": "<issuance txid>",
    "cids": "<metadataCid>,<mediaCid>" }
  ```
  `cids` is a **comma-separated** list (metadata first, then any media/sub-files).
- **Behaviour:** each CID is inserted (idempotent — `INSERT OR IGNORE`) on the
  current open **frontier page** (`getWritablePage()`), the page clients are
  parked on, so nodes pick it up on their next `~10 min` fetch without a
  restart. Pages roll to a fresh one after 500 entries.
- **Response:** `{"ok":true,"page":<n>,"added":<count>}`.

Verify it landed: `GET /permanent/<page>.json` should list the `assetId-txHash`
key with its CIDs; then watch a subscribed node pin them.

---

## Node operator setup (earning DGB)

1. Run kubo (IPFS) and `DigiAssetWindows` as usual.
2. **Forward TCP port 4001** — strongly recommended (see the port-4001 section
   above). Forwarded nodes verify instantly; NAT'd nodes can still qualify via
   the fallback but less reliably.
3. Point the node at the pool by adding to `config.cfg`:
   ```
   psp0subscribe=1
   psp0payout=YOUR_DGB_ADDRESS       # pool 0 = local pool (no server); needs a payout
   psp2server=https://pool.digistamp.co
   psp2subscribe=1
   psp2payout=YOUR_DGB_ADDRESS       # pool 2 = DigiStamp, this pool
   ```
   (New installs are prompted for the address in the first-run wizard and default
   to the DigiStamp pool, so most users don't need to edit anything by hand.
   Pool 1 was MCTrivia's PSP — now deprecated; nodes already on `psp1` keep working.)
4. Set a **real** payout address for BOTH pools. If `psp0payout`/`psp2payout` are
   left as the `_psppayout` label, the node tries to mint an address from a loaded
   DigiByte wallet and errors with `Could not generate new PSP payout address`
   when none is loaded. Watch the DigiAssetWindows dashboard's Payment row:
   `registered (no payouts yet)` means the pool hasn't enabled payouts; `active`
   means it has.

## Advertising your pool ("Join my pool" snippet)

There is **no in-app pool discovery** — a node only finds your pool if you tell
people the URL. Copy-paste this to Discord/Twitter/your site:

> **Join the DigiStamp Permanent Storage Pool and earn DGB for hosting DigiAssets.**
> 1. Install DigiAsset for Windows: https://github.com/chopperbriano/DigiAssetWindows
> 2. When the setup wizard asks for a pool, press Enter to accept the default
>    (`https://pool.digistamp.co`) — or add `psp2server=https://pool.digistamp.co`
>    to your `config.cfg`.
> 3. Forward IPFS port 4001 on your router for reliable verification.
> 4. Restart. Your node registers automatically; watch the dashboard's Payment row.

## Publishing the pool (TLS + landing page)

Clients default to `https://pool.digistamp.co`, but the pool exe only speaks
plain HTTP on 14028. Use the Caddy reverse-proxy bootstrap in
**[deploy/](deploy/README.md)** to put a real HTTPS front end and a landing page
in front of the exe with one script.

---

## Notes / current limitations

- **Verification is hybrid** — direct dial first, then a DHT provider-record +
  bitswap-fetch fallback for NAT'd nodes (see the port-4001 section). The
  fallback proves the node *announced* and the content is *retrievable*, not
  that this specific node served the bytes; a determined actor could game it, so
  it's a reasonable v1 for an operator-approved pool but not Sybil-proof.
- **Reward is weighted** by each node's `coverage x reliability` (coverage =
  fraction of sampled permanent CIDs it provably provides; reliability = verify
  pass-rate over recent rounds). A partial or flaky host earns proportionally
  less; a reachable-but-not-hosting node earns nothing. See **[FAIRNESS.md](FAIRNESS.md)**.
- The pool exe is **Windows-only** for now (`if(WIN32 AND MSVC)` in CMake).
