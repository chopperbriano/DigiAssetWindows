# Permanent Storage, the "tracked" list, and what "coverage" really means

If your node shows something like:

```
Asset index:  1 local / 3442 tracked / 0.0% coverage
Log: WARNING: Permanent-list coverage: 0/3442 (3442 missing)
     WARNING:   missing asset: La2iyS7ug9PrhrT7HCG6sQBXzujrP7dCCBP5ET
```

…and you're still syncing, **nothing is wrong.** This document explains exactly
what those numbers are, why they look the way they do during sync, and the one
situation where they *would* indicate a problem.

---

## 1. What "permanent storage" is

DigiAssets store their metadata and media as files on **IPFS** (content addressed
by a CID). IPFS only keeps a file around while *someone* is "pinning" it. If every
holder of an asset stops pinning its files, the content can disappear even though
the asset still exists on-chain.

To stop that from happening, the DigiAsset ecosystem runs a **Permanent Storage
Pool (PSP)**: a group of nodes that agree to permanently **pin** the files for a
curated set of assets, so that content is always available from *somebody*. Your
node, by joining the pool, becomes one of those hosts.

## 2. The "tracked" list (the 3442)

**`3442 tracked`** = the number of assets currently on the pool's **permanent
list** — the canonical set of assets enrolled for permanent storage.

- It is published by the pool as paginated JSON: `<pool>/permanent/0.json`,
  `/permanent/1.json`, … Each page's `changes` map is `"<assetId>-<txhash>": [CID, CID, …]`.
- Your node fetches these pages and **pins every CID** to your local IPFS. That
  pinning is the *actual hosting work*, and it happens on its own background
  thread — **it does not depend on your blockchain sync.**
- The list is **not fixed** — it *grows* as new assets are enrolled. The node
  walks forward a page at a time and remembers where it got to
  (`psp1permanentpage` in `config.cfg`). "3442" is just today's size.

So "tracked" answers: *"how many assets is the pool asking every node to host?"*

## 3. Three different things are all loosely called "coverage"

This is the part that causes confusion. There are **three separate measurements**,
and the node dashboard shows the one that looks scariest during sync:

| # | What it measures | Where | Depends on chain sync? |
|---|---|---|---|
| **A. Decode coverage** | How many tracked assets your node has **decoded from the blockchain** (are in your local DB) | The dashboard `% coverage` line + the "missing asset" log warnings | **Yes** — this is really a sync-progress meter |
| **B. Pinning** | Whether your node has **pinned the tracked CIDs** in IPFS (the real hosting) | Background PSP fetcher thread (`ipfs pin`) | **No** — uses the CID list directly |
| **C. Verified coverage** | Whether the pool can **prove your node serves** the content (via IPFS provider lookup) | The **pool server**, and it's what payouts are weighted by | **No** |

The dashboard's **`% coverage`** and the **"missing asset"** warnings are **A** —
*decode* coverage. For each asset on the tracked list, the node asks *"have I
decoded this asset from the chain yet?"* ([`checkPermanentCoverage`](src/ConsoleDashboard.cpp)).

## 4. Why it's 0% / "all missing" while syncing (and why that's fine)

DigiAsset issuances don't begin until block **8,432,316**. If your node is at, say,
block 5,135, it hasn't reached the blocks where *any* of those 3442 assets were
issued — so none are in your local DB yet, and measurement **A** reads 0%. Every
tracked asset shows "missing" simply because **you haven't gotten to it in the
chain yet.**

Meanwhile, measurement **B** (the actual pinning) may already be running fine in
the background — the node pins the tracked CIDs straight from the list without
needing to decode the chain first.

As you sync past 8,432,316 and onward, decode coverage climbs toward 100%. On a
fully synced node it should sit at (or very near) 100%.

> Since win.86 the node knows the difference: while syncing it shows
> `… % decoded (syncing)` in a calm colour and logs a single quiet line, instead
> of red "0.0% coverage" and a wall of "missing asset" warnings.

## 5. When it *is* worth investigating

Decode coverage is only a red flag when **all three** are true:

1. Your node is **fully synced** (dashboard `Block:` ≈ chain tip, "Fully Synced"), **and**
2. Coverage is still well below 100%, **and**
3. It stays that way across several 10-minute check cycles.

That would mean the chain analyzer decoded the chain but somehow doesn't have
assets the permanent list references — worth reporting. **Anything you see at 0%
mid-sync is normal and self-heals.**

If instead your concern is *"am I actually hosting / getting credit?"*, that's
measurements **B** and **C**, not the dashboard's coverage line:
- **B (pinning):** check the DEBUG log for `PSP permanent page N fetched (… CIDs, … pin failures)`.
- **C (verified):** check the pool's node list / your node's verified status on the pool site — that's what payouts are based on.

---

### TL;DR
- **tracked** = assets the pool asks you to host (a growing, curated list).
- **coverage % / "missing"** on the dashboard = how far your **chain sync** has
  decoded that list. 0% mid-sync is normal; it fills in as you catch up.
- **Actual hosting** (pinning) and **getting paid** (pool verification) are
  separate mechanisms that don't wait for your sync.
