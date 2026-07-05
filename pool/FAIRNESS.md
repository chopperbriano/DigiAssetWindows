# Pool payout fairness

How the DigiStamp pool decides **who gets paid and how much**, so payouts reward
*actual hosting*, not just showing up.

## The model

Each payout, every eligible node's share is:

```
share_i = budget * weight_i / Σ weight
weight  = coverage × reliability          (both 0..1)
```

- **coverage** — of a random sample of the pool's permanent CID list, what fraction
  does the node **provably provide**? This is *how much of the content it hosts*.
- **reliability** — over recent verify rounds, how consistently did the node pass
  verification? This is an *uptime / consistency* proxy.

A node hosting 100% of the list, always up, earns the most. A partial or flaky
host earns proportionally less. A node that's reachable but **hosts nothing**
scores `coverage ≈ 0 → weight 0 → paid nothing**, even though it pings.

## Eligibility gate (must pass all)
From `getVerifiedPayoutTargets()`:
- seen (keepalive) within **7 days**, AND
- passed dial-back verification within **24 h** (`lastVerifyOk`), AND
- fewer than **3** consecutive verification failures, AND
- **`coverageScore > 0`** — has demonstrably provided sampled content.

## How the scores are measured (Phase 1 — implemented)
The `PoolVerifier` background loop (~every 60 s) does this per round:
1. Samples **6 random CIDs** from `permanent_assets` and, for each, asks the DHT
   who provides it (`findProviders`), gated on at least one being `fetchable`
   (so dead content / a dead local IPFS node can't score anyone).
2. For each node it's checking, **coverage this round** = fraction of those
   sampled CIDs whose provider set includes the node's peerId. (A same-machine
   node serves everything → 1.0.)
3. Folds that into the node's EMA `coverageScore`, and folds the pass/fail of the
   reachability probe into `reliabilityScore` (`updateNodeScores`, smoothing
   `A = 0.1`). Coverage is only updated when there were live samples, so a
   measurement gap never penalizes a node.

Scores live in the `nodes` table (`coverageScore`, `reliabilityScore`, REAL 0..1),
added by a `pool.db` migration. The dashboard's `[P]`/`[E]` show each node's
`cover %` and `uptime %` next to its amount.

### Phase 1 limitation (be honest)
Coverage is inferred from **DHT provider records**, which prove a node *announces*
it provides a CID (stock kubo only reprovides what it actually pins) — not a
cryptographic proof of possession. A custom/modified node could announce without
storing. Stock nodes can't, so this is a big improvement over "reachable = paid",
but it isn't unforgeable.

## Phase 2 — cryptographic proof of storage (planned, not yet built)
Closes the "announce without storing" gap. Requires new code in **both** the pool
and the DigiAsset node, released together:

1. The pool picks a random CID the node should hold and a random **byte range**.
2. It challenges the node: "return the hash of bytes `[a,b)` of CID `X`."
3. The node must hold the data to answer; the pool verifies against its own copy.
4. Pass/fail feeds the same `coverageScore` (replacing/augmenting the DHT signal),
   so the weighting math is unchanged — only the coverage *measurement* gets
   stronger.

Until Phase 2 ships, Phase 1's sampling is the fairness mechanism.

## Tuning knobs (code constants today; could move to `pool.cfg`)
- Sample size per round (**6** CIDs) — higher = less noisy coverage, more DHT load.
- EMA smoothing (**A = 0.1**) — higher = reacts faster, lower = steadier/longer memory.
- Verify batch/interval (**10 peers / ~60 s**).
