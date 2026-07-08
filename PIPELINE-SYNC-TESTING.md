# Pipeline Sync — Benchmark & Validation Guide

`pipelinesync` overlaps **block fetch (RPC)** with **block processing (DB writes)**
during deep initial sync: a background thread pre-fetches the next block(s) while
the main thread commits the current one. Default **off**. This guide lets you
prove it's *faster* **and** *correct* **and** *stable* while paying the sync cost
only **once**.

> **Prereq:** DigiByte Core fully synced (this is the slow part — do it once and
> leave it running; the node only reads from it over RPC). Node **not** running.
> Use a config with `verifydatabasewrite=0` so both runs share the fast pragmas.

---

## The trick: one sync, then A/B a fixed window with a DB backup

You only need to reach an asset-era height **once**. Then you replay the *same*
block window twice (pipeline off, then on) by restoring a backup between runs, so
it's a true apples-to-apples comparison over identical data.

Asset processing only starts at height **8,432,316** (below that is fast
header-only sync). The pipeline only engages when **>110 blocks behind** and in
the asset era — so pick a window comfortably inside the asset era and far from tip.

### Step 1 — Sync once to a start height (the only slow step)
1. Delete/rename any existing `chain.db` in `C:\DigiAssetWindows` (fresh start).
2. In `config.cfg` set `pipelinesync=0` and `verifydatabasewrite=0`.
3. Start the node. It blows through the pre-asset headers fast, then starts asset
   processing at 8,432,316. Let it reach, say, **8,450,000** (watch the console's
   height), then **stop the node** (close it / Ctrl-C).

### Step 2 — Back up the DB at the start height
```powershell
Copy-Item C:\DigiAssetWindows\chain.db C:\DigiAssetWindows\chain_START.db -Force
```
(There is no `-wal`/`-journal` file to copy — `journal_mode=MEMORY`.)

### Step 3 — Baseline run (pipeline OFF)
1. Ensure `pipelinesync=0`.
2. Start the node. Note the **start time and height** from the console.
3. Let it advance a fixed window — **10,000 blocks** is plenty (e.g. to 8,460,000).
4. Note the **end time**. Compute blocks/sec = 10000 / seconds. **Stop the node.**
5. Record the correctness fingerprint at this height (see “Correctness” below).

### Step 4 — Restore the DB
```powershell
Copy-Item C:\DigiAssetWindows\chain_START.db C:\DigiAssetWindows\chain.db -Force
```

### Step 5 — Pipeline run (pipeline ON)
1. Set `pipelinesync=1`.
2. Start the node. In the log you should see **`Pipeline sync ENABLED`** — that
   confirms it actually engaged. (If you don't see it, you're not >110 behind or
   not in the asset era.)
3. Let it advance the **same 10,000-block window**. Note end time → blocks/sec.
4. **Stop the node.** Record the same correctness fingerprint.

### Step 6 — Compare
- **Speed:** ON blocks/sec vs OFF blocks/sec. Overlap should push wall-clock toward
  `max(rpc_time, db_time)` instead of `rpc_time + db_time`.
- **Correctness:** the fingerprints from Step 3 and Step 5 **must be identical.**

---

## Reading the speed (two easy meters)

**Per-100-block line** (always logged): each 100 blocks logs
`... in <N> ms per block - <T> left to sync`. Lower `ms per block` = faster.

**The PERF line** (logged every 500 blocks, DEBUG) is the most useful — it shows
*why*:
```
PERF: <ntx> txs rpc=<A>ms db=<B>ms tx/blk=<n>
```
- `rpc` = time spent fetching blocks, `db` = time spent writing.
- **Serial (off):** wall-clock ≈ `rpc + db`.
- **Pipeline (on):** wall-clock ≈ `max(rpc, db)` — the overlap. If `rpc` and `db`
  are similar, expect close to ~2×. If one dominates, the win is smaller (you can't
  overlap away the bigger of the two). This line tells you the ceiling before you
  even run it.

Set `showallblocksynctimes=1` if you want a per-block time line instead of per-100.

---

## Correctness (the gate that matters most)

A faster sync that computes different asset state is worthless. After each run,
with the node still running (or restarted, pointed at the same chain.db), query
its RPC at the identical height:

```powershell
C:\DigiAssetWindows\DigiAssetWindows-cli.exe getnodestats
```
Record **`syncHeight`**, **`assetCount`**, and **`topAssetIndex`**. The OFF run and
the ON run, taken at the **same `syncHeight`**, must produce **identical
`assetCount` and `topAssetIndex`.** If they differ, the pipeline corrupted
interpretation — stop and report it (this must never happen; the pipeline only
changes *when* a block is fetched, not *what* is computed).

---

## Stability tests (do these once with pipeline ON)

The original pipeline was pulled for three failures; each has a matching test:

1. **Crash/overrun (the old buffer bug):** just running the full 10k-block window
   with `pipelinesync=1` exercises it — each prefetched block now carries its own
   TX data, so an in-flight prefetch can't clobber the block being processed.
   Success = it completes with no crash.

2. **Silent RPC-failure hang:** while syncing with pipeline ON, **stop DigiByte
   Core** (or pause it) for ~30s, then restart it. The node must **log an error and
   keep control** (retry/recover) — **not hang**. Confirm the console isn't frozen
   and the process is still responsive; it should resume once Core is back.

3. **Clean shutdown mid-prefetch:** while pipeline is ON and actively syncing,
   **stop the node** (Ctrl-C / close). It must exit within a few seconds (the
   producer thread is joined) — no hang, no crash. Restart it: it repairs the last
   partial block and resumes. Run `getnodestats` after resume and confirm
   `assetCount` still matches the baseline at that height.

**Reorgs:** you don't need to test these against the pipeline. It only runs when
>110 blocks behind, where a reorg cannot reach the sync frontier. Within 110 of the
tip — where reorgs actually happen — the node runs the unchanged serial path with
its per-100-block fork check.

---

## If it wins and it's clean

If ON is meaningfully faster **and** all three stability tests pass **and** the
correctness fingerprints match, we flip `pipelinesync` to default-on in a follow-up
release. If it's not faster (e.g. `db` dominated `rpc` on your hardware), we leave
it off — no harm done, since it's opt-in.

## Quick reference
| | |
|---|---|
| Enable | `pipelinesync=1` in `config.cfg` |
| Confirm engaged | log line `Pipeline sync ENABLED` |
| Speed meter | `PERF: ... rpc=..ms db=..ms` (every 500 blocks) |
| Correctness | `getnodestats` → `assetCount`/`topAssetIndex` identical at same height |
| Engages only | >110 blocks behind, height ≥ 8,432,316 |
| Read-ahead depth | 4 blocks (bounded) |
