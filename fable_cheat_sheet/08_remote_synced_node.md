# Remote Synced DigiByte Node (10.0.3.50) — Live-Chain Testing Unblocked

*Written 2026-07-18 by another agent, while the local Mac's DigiByte node was still
stuck resyncing. This documents a second, fully-synced node built on a different
machine specifically so live-chain RPC testing doesn't have to wait on the local
resync anymore.*

## TL;DR

There is now a fully synced, txindex-enabled DigiByte Core v8.22.2 node reachable
from this Mac at `10.0.3.50:14022`. It has **no wallet funds** (see below), so it
does not unblock wallet-balance/send/issue live tests — but it DOES unblock any
test that only needs chain data, most importantly the
`DigiAssetTransaction.existingAssetTransactions` replay test
(`DigiByteTransaction(txid, height)` just does RPC tx lookups, no wallet needed).

## Connection details

```
host:         10.0.3.50
port:         14022
rpcuser:      digiasset_remote
rpcpassword:  (see /home/mctrivia/digibyte-node-data/digibyte.conf on that host,
               or ask the user — not duplicating the live secret into a repo file)
```

Reachable directly from **this Mac** (`10.0.25.86`) over the LAN — the remote
node's `rpcallowip` is scoped specifically to that IP, not open broadly. If you're
running tests from a different machine, you'll need to either SSH-tunnel through
this Mac, or add your IP to `rpcallowip=` in
`/home/mctrivia/digibyte-node-data/digibyte.conf` on `10.0.3.50` (ssh user
`mctrivia`, passwordless sudo available) and restart `digibyted` there.

Quick verification:
```bash
curl -s --user "digiasset_remote:<password>" \
  --data-binary '{"jsonrpc":"1.0","id":"t","method":"getblockchaininfo","params":[]}' \
  -H 'content-type: text/plain;' http://10.0.3.50:14022/
```
Should show `"initialblockdownload":false` and blocks in the 23.8M+ range.

## How to point this repo's code at it

`src/DigiByteCore.cpp` reads the RPC host from `config.cfg`'s `rpcbind` key
(defaults to `127.0.0.1`). Both `config.cfg` (repo root) and `bin/config.cfg` have
the same four keys to edit:

```
rpcbind=10.0.3.50
rpcport=14022
rpcuser=digiasset_remote
rpcpassword=<see above>
```

**Remember to revert (`rpcbind=127.0.0.1`, local rpcuser/rpcpassword) if/when you
want to go back to testing against the local node** — don't leave the repo
pointed at the remote box by accident. Consider keeping a `config.cfg.remote`
copy alongside if you'll be switching back and forth.

## Important: wallet is empty

This node was built with wallet support compiled in (`--enable-wallet
--without-bdb --with-sqlite=yes`, `disablewallet=0` in its conf) — RPC wallet
methods exist and won't error with "no wallet" — but the wallet itself has
**no keys, no funds, no transaction history**. The original wallet.dat/keys were
deliberately NOT copied to this host (it's a shared machine running other
people's production inference workloads; didn't want private keys sitting on it).

So: `getwalletbalances`, `issueasset`, `sendasset` end-to-end happy-path tests
still need the **local** node + its funded wallet, once that resync finishes.
This remote node only helps with read/chain-query tests.

## What this unblocks right now

- `DigiAssetTransaction.existingAssetTransactions` (the 1-2h replay test, see
  `05_tests_and_psp.md` and `LAST_TASKS_NOTES.md` "Remaining work" #2) — it died
  previously with "Core Offline" because the local node was mid-resync at block
  1.18M of 23.8M. Point config.cfg at 10.0.3.50 (above) and it should now be able
  to fetch every tx it needs.
- Any `RPCMethodsTest.*` / `PermanentStoragePool.mctrivia_allAddressesRecognized`
  work that depends on `tests/testFiles/rpcTest.db` being populated (that db is
  produced BY the replay test above — so this is really the same unblock).
- Any ad-hoc `getrawtransaction`/`decoderawtransaction`/block-explorer-style RPC
  work against real chain data.

## Host context — this is a shared machine, be careful

`10.0.3.50` (hostname `gx10-9f89`) is an NVIDIA DGX Spark also running a
production vLLM inference server (`vllm.service`, port 8200) and a priority-queue
middleware in front of it (`vllm-queue.service`, port 8080, see `~/vllm-queue/`
on that host for its own CLAUDE.md). Both were briefly stopped/drained while this
node did its initial sync and reindex, then restarted — they're back to normal
operation now. **Do not stop, restart, or otherwise touch those services** as
part of testing this repo; this node runs alongside them, not instead of them.
If you ever need to restart just the DigiByte node:
```bash
ssh mctrivia@10.0.3.50 '~/digibyte-src/src/digibyted -datadir=/home/mctrivia/digibyte-node-data -daemon'
```
(no `-reindex` needed — the index is clean).

Mempool on this node is whatever it's accumulated since its own restart — don't
expect deep unconfirmed-tx test coverage immediately after a restart.

## How it was built (background, only useful if this ever needs repeating)

Copied `blocks/`, `chainstate/`, `indexes/` from the local Mac node's **live,
still-running** datadir via rsync (42GB). Copying a live LevelDB database this
way is not safe — it came out corrupted (`Corruption: 1 missing files`) because
the source was being written to mid-copy. Fixed by starting the new node with
`-reindex`: this rebuilds the block index + chainstate + txindex/blockfilterindex
from the already-downloaded raw `blk*.dat` files, no network re-download needed.
Took about 8 hours on this host's 20 cores. If you ever need to do this again,
either stop the source node first (cleanest) or expect to `-reindex` afterward.
