# Chain-split 2026 regression fixtures

Real on-chain fixtures for the two correctness hazards that came out of the
**DigiByte v7 / v8 / v9 chain split of June 2026**, captured from a synced
`DigiByte:9.26.4` node (the canonical/fixed chain). These are the shared
regression fixtures for #3 (the "two problem blocks") and #4 (v7/v8 parsing) —
**determined without needing anyone's block numbers**, by combining public
incident info with a scan of the live chain.

## What happened (June 28 2026)

An actor **reactivated the retired Groestl algorithm** and mined at floor
difficulty starting at **block 23,751,096**. Root cause: the consensus rule that
rejected retired-algo blocks existed in v7.17.3 but was **accidentally dropped in
the v8 Bitcoin-Core rebase (2021/22)** — so v8/v9 software *accepted* Groestl
blocks while v7.17.3 *rejected* them, splitting the network. ~1,356 Groestl
blocks were mined; v9.26.3+ restores the reject rule so the network reconverges.

**Key finding from scanning the live v9.26.4 chain:** the canonical chain **still
contains the Groestl blocks** — they were NOT reorged out. The first is block
**23,751,096** (hash `b2749cf0462eb6c35bf5f8f1d75f4891e62d3ca1d9861fac48b46c05ba078283`),
and every Groestl block carries an **anomalous legacy version `0x00000400`**
(1024) instead of the normal `0x20000xxx`. That unusual version is the most
likely thing that stalls an older DigiAsset indexer that assumes the modern
version/algo encoding. The merged Windows indexer syncs cleanly past them.

## #3 — Groestl block fixtures (`groestl-blocks/`)

`block-23751096.json`, `block-23751103.json`, `block-23751109.json` — full
`getblock <hash> 2` output of the **first three Groestl blocks**. All are
coinbase-only (`nTx=1`), version `0x00000400`, `bits=1e0fffff` (floor).

**Regression assertion:** an indexer must process these blocks (advance past the
coinbase, apply the reward UTXO) **without stalling on the unfamiliar version /
algo**. Our v9 indexer does; an older indexer that hard-validates block version
is the failure mode. Point Matt's Linux indexer at 23,751,096 first.

## #4 — v7/v8 scriptPubKey fixtures (`scriptpubkey/`)

Same v8 rebase changed `getrawtransaction`'s address encoding:
- `v7-addresses-plural.json` — v7.17.x shape: `reqSigs` + `addresses[]`, no `address`.
- `v8-v9-address-singular.json` — real v9.26.4 shape: singular `address` + `desc`, no `addresses`.

**Regression assertion:** the parser must extract the **same address** from both.
The merged code does this in `src/DigiByteCore.cpp:311-319` (reads `addresses[]`
*and* singular `address`, deduped).

## Regenerating

`node/capture-chain-split-fixtures.ps1` re-pulls the block fixtures + a fresh
v8/v9 scriptPubKey from a synced Core (reads RPC creds from `config.cfg` or
`%APPDATA%\DigiByte\digibyte.conf`). The v7 fixture is hand-authored (no v7 node
is available) from the documented v7.17.x format.
