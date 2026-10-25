# Pool payout fairness

How the DigiStamp pool decides **who gets paid and how much**, so payouts reward
*actual hosting*, not just showing up, and stay worth having as more nodes join.

The rules live in `pool/StoragePlan.h` (pure, unit-tested in
`tests/StoragePlanTest.cpp`). The budget is unchanged: a share of the pool
wallet per period (`poolpayoutpercent` or `poolspendperperiod`). This decides
who it goes to.

## The model

Each payout period:

1. **A capped paid set.** Eligible nodes (gate below) that hold something are
   ranked by reliability, then by how long they have served, and the top
   `poolmaxpaidnodes` are paid. The rest wait and keep building their scores; a
   paid node that drops out is replaced by the next. `0` means no cap.

   This is what stops dilution. Before, every node stored everything and
   shared one pot, so each new node shrank every share. With a cap, the pot is
   split among a fixed number of nodes however many join.

2. **A fixed number of paid copies per file.** Each CID is assigned to
   `poolreplicas` nodes of the paid set by rendezvous hashing: the nodes with
   the highest `hash(cid|peerId)` win. Assignments are stable, so losing or
   adding a node moves only the files that node held.

3. **Paid by bytes.** The verifier measures each file's size from the pool's
   own IPFS (`files/stat`), a few per minute. A node's weight is

   ```
   weight = bytes assigned x held x reliability
   share  = budget x weight / sum of weights
   ```

   so a node holding a large video earns more than one holding a thumbnail. A
   file not measured yet counts as 1 byte until it is.

4. **`held` comes from storage challenges** when the node has answered any
   (below), and otherwise from its DHT coverage times `poollegacyweight`
   (default 0.5). An older client that cannot answer challenges still earns,
   but half what a proving node does. Once a node has answered a challenge it
   is judged by its answers only.

## Eligibility gate (must pass all)
- seen (keepalive) within **7 days**, AND
- passed dial-back verification within **24 h** (`lastVerifyOk`), AND
- fewer than **3** consecutive verification failures, AND
- holds something: `held > 0` (passed challenges, or DHT coverage on an older
  client). A node that holds nothing cannot take a paid slot.

## Storage challenges (implemented)

Coverage from DHT provider records proves a node *announces* a file, not that it
stores it. Challenges close most of that gap:

1. On each keepalive (about every 20 minutes) from a registered node, the pool
   adds a `challenge` to its reply: a CID the node is assigned, and a random
   byte range of up to 64 KiB. Older clients look only for the reply's usual
   text and ignore the field.
2. The node checks the file is **pinned locally** (it never fetches a file to
   answer), reads that range from its own IPFS, and posts the SHA-256 to
   `POST /proof` with its peerId and secret.
3. The pool reads the same range from its own IPFS and compares. Each verdict
   moves the node's `proofScore` (EMA, 0.2 per answer).
4. An answer is accepted once, within **120 seconds**. A node that has
   answered before and ignores a challenge is scored a fail, so going quiet on
   a file you dropped does not protect your score. A node that has never
   answered (an older client) is not penalised; `poollegacyweight` covers it.
5. If the pool cannot read the file itself, it scores neither way.

**What this does not prove.** A node that stores nothing could fetch the file
from the network inside the deadline to answer. That costs it the bandwidth of
holding the file, and the answer only comes from pinned files on an honest
client, so cheating costs about what honest storage costs. It is not a
cryptographic proof against a determined cheat with a fast connection.

## Tuning knobs

| `pool.cfg` key | Default | |
|---|---|---|
| `poolmaxpaidnodes` | `10` | Paid nodes per period; `0` = no cap, which brings dilution back. |
| `poolreplicas` | `3` | Paid copies of each file. |
| `poollegacyweight` | `0.5` | Weight kept by a node that never answers challenges. |
| `poolchallenges` | `1` | `0` turns challenges off. |

Code constants: challenge range 64 KiB, deadline 120 s, proof EMA 0.2
(`StoragePlan.h`); DHT sample 6 CIDs per round and coverage/reliability EMA 0.1
(`PoolVerifier.cpp`, `PoolDatabase.cpp`).

## Not built yet

- **Holder priority for paid slots.** The marketplace has no holder tiers yet.
  When it does, the paid-set ranking can read a node operator's tier first.
- **Nodes storing only their assignment.** Clients still pin the whole list;
  they are paid only for the bytes assigned to them. A client that pins just
  its assignment would let small machines take part.
