# Pre-Deployment Audit Report — DigiAsset for Windows

_Adversarially-verified findings, de-duplicated. Date: 2026-07-03._

---

## 1. Executive Summary

| Severity (verified) | Count |
|---|---|
| High | 6 |
| Medium | 9 |
| Low (code/hardening) | 29 |
| Doc ↔ code consistency | 9 |
| **Total unique findings** | **53** |

**MUST-FIX before deploy: 8 items** (every high-severity finding plus 2 medium findings flagged `mustFixBeforeDeploy`).

Theme of the blockers: **untrusted on-chain data parsed with unchecked vector/array indexing → remote node-crash DoS** (4 findings in the asset-rule/transfer decoders), plus the **public pool HTTP server having no body-size cap and no socket read timeout** (memory-exhaustion + slowloris), the **asset RPC binding all interfaces**, and an **unescaped attacker-controlled payout address concatenated into a money-moving RPC call**. None are confirmed RCE or fund-theft; all are availability/exposure defects with cheap fixes.

Everything in sections 3–5 is real but non-blocking under the deployed threat model (asset RPC localhost/operator-only, pool HTTP behind Caddy, small operator-run pool, payouts off by default in Phase 1).

---

## 2. MUST-FIX BEFORE DEPLOY

> Ordered: remote-untrusted-input crashes first, then public pool-server DoS, then exposure/injection.

### ☐ 1. OOB vector read in asset-transfer decode (percent branch) — remote node crash
- **`src/DigiByteTransaction.cpp:328`** (decodeAssetTransfer)
- **Scenario:** The `percent` branch reads `inputs[index][0].getCount()` *before* the `index >= inputs.size()` guard at lines 341/350. A crafted OP_RETURN with one more percent-flagged transfer instruction than there are input asset groups makes `index == inputs.size()`, so `inputs[index]` is an OOB `operator[]` read (UB, likely segfault) on both the transfer and issuance paths. The `catch(out_of_range)` at line 290 is inert — `operator[]` does not throw. Any peer who lands the tx in a block crashes every node that processes it.
- **Fix:** Hoist `if (index >= inputs.size() || inputs[index].empty()) throw exceptionInvalidTransfer();` above the `amount` computation (before line 327), or guard the percent branch specifically.

### ☐ 2. Unbounded `vout[]` index in approval-rule decode — remote node crash
- **`src/DigiAssetRules.cpp:126`** (decodeApproval; range case 124–131, per-output case 145–147)
- **Scenario:** `outputNum` / `start` / `length` come from `getFixedPrecision()` (attacker value up to ~1.8e16) and index `txData.vout[outputNum].scriptPubKey.addresses[0]` with no bounds check. Reached for every opcode 3/4 issuance tx. `operator[]` OOB is UB, not caught by `processIssuance`'s `catch(out_of_range)` (DigiAsset.cpp:487). `addresses[0]` also unchecked for empty (nulldata) outputs.
- **Fix:** Before indexing, validate `outputNum < vout.size()` (and `start+length <= vout.size()`, guarding unsigned overflow) and `addresses` non-empty; `throw out_of_range` otherwise. Mirror the guard already in `decodeRoyalties` (line 199).

### ☐ 3. Unbounded `vout[]` index + 7-bit static-array overrun in vote/expiry decode — remote node crash
- **`src/DigiAssetRules.cpp:268`** (decodeVoteAndExpiry)
- **Scenario:** (a) `voteStart` from `getFixedPrecision()-1`; loop 268–270 indexes `vout[outputNum]` with no bounds check. (b) `voteLength = getBits(7)` (0–127); the default-list branch (259–261) reads `standardVoteAddresses[i]` but the array has only 50 entries → OOB for 51–127. Both UB, not caught. `.addresses[0]` empty-vector hazard at line 270.
- **Fix:** Verify `voteStart >= 0 && voteStart+voteLength <= vout.size()` and target `addresses` non-empty before the output loop; reject `voteLength > 50` before the default-list loop; `throw out_of_range`.

### ☐ 4. OOB static-array + `vout[]` reads in royalty-units decode — remote node crash
- **`src/DigiAssetRules.cpp:170`** (decodeRoyaltyUnits)
- **Scenario:** (a) `standardExchangeRates[getBits(7)]` indexes a 20-element array of `ExchangeRate` (with `std::string` members) with a 0–127 value → copies garbage into `std::string`, near-certain crash. (b) `output = getFixedPrecision()` then `vout[output].scriptPubKey.addresses[0]` unchecked. Reached via royalty-units rule 0x9 on issuance. UB, not caught.
- **Fix:** `throw out_of_range` if `getBits(7) >= 20` before indexing `standardExchangeRates`; validate `output < vout.size()` and `addresses` non-empty in the non-standard branch.

### ☐ 5. Pool HTTP body read has no size cap — unauthenticated memory-exhaustion DoS
- **`pool/PoolServer.cpp:518`** (handleConnection; Content-Length parsed at 510, body loop 517–524)
- **Scenario:** Headers cap at 16 KB but the body does not. `Content-Length` is taken verbatim via `std::stoul`; the loop appends to a `std::string` with no bound. The deploy Caddyfile (`pool/deploy/Caddyfile:25`) is a bare `reverse_proxy` with no `max_size`, so it streams the uncapped body to the backend. An unauthenticated POST (even to a 404 path — body is read before dispatch) with a multi-GB Content-Length drives the 8-thread pool toward OOM, disconnecting all miners. (RAM tracks bytes actually sent — volumetric, not single-packet.)
- **Fix:** Before the loop, reject `Content-Length` above a small cap (e.g. 64 KB) with 413; also enforce the cap on accumulated `body.size()`. Optionally add `request_body { max_size 64KB }` to the Caddyfile.

### ☐ 6. Pool HTTP server has no socket read/idle timeout — unauthenticated slowloris/RUDY DoS
- **`pool/PoolServer.cpp:470`** (header loop 468–478, body loop 518–523; fixed 8-thread pool at 400)
- **Scenario:** `read_some()` with only an `error_code`, no deadline anywhere. Caddy fully parses headers so the classic no-`\r\n\r\n` variant is absorbed, but a **slow-body/RUDY** attack (complete headers, large Content-Length, trickle the body to a proxied path) reaches the backend — Caddy streams bodies with no timeout. 8 such connections park all 8 workers forever; every new connection queues and the API goes offline until restart. Acceptor also binds `tcp::v4()` (0.0.0.0), so the literal attack works too if 14028 isn't firewalled.
- **Fix:** Set `SO_RCVTIMEO`/`SO_SNDTIMEO` (5–10 s) on the accepted socket, or convert to async with a `steady_timer` that cancels on expiry; cap total connection time and concurrent connections. Bind the acceptor to 127.0.0.1 (Caddy is the only intended client).

### ☐ 7. Asset JSON-RPC binds 0.0.0.0 — wallet money-movement RPC exposed with cleartext Basic Auth
- **`src/RPC/Server.cpp:161`** (`tcp::endpoint(tcp::v4(), _port)`)
- **Scenario:** Bound to all interfaces unconditionally, no loopback option, no per-connection IP check — contradicting the documented localhost-only design (the Core client at DigiByteCore.cpp:111 defaults to 127.0.0.1). Only protection is Basic Auth over plain HTTP (no TLS), and shipped `config.cfg:6` sets `rpcallow*=1`, exposing `send`/`sendmany`/`sendtoaddress`/`getencryptedkey` (forwarded to Core's wallet). The installer's program-scoped firewall rule (`setup-digiasset.ps1:721`, `-Profile Any`) opens all ports for the node exe, so any LAN host can reach 14024, sniff or brute-force the password, and drain the wallet.
- **Fix:** Bind 127.0.0.1 by default via a configurable bind address (`make_address(bindAddr)`), optionally enforce an IP allowlist before auth. Ship `config.cfg` with `rpcallow*=false` (match example.cfg). Narrow the installer firewall rule so it doesn't expose 14024 beyond loopback.

### ☐ 8. Attacker-controlled payout address concatenated unescaped into `sendtoaddress` RPC — JSON/param injection on a money path
- **`pool/PoolDashboard.cpp:115`** (sendToAddress builds `"params":["+address+","+amountBuf+"]`)
- **Scenario:** `address` is the raw HTTP `address`/`payout` field from the public `/keepalive` & `/list` endpoints, stored unvalidated by `upsertNode` (PoolServer.cpp:779/797). No `validateaddress`, no escaping. A payload like `DValidAddr",50000,"c",...,` produces *well-formed* JSON with a valid `param[0]` and an attacker-chosen amount shifted into a later numeric slot — a plausible overpay/param-injection, not merely malformed JSON. Payouts are manual (double-confirmed) and Core may reject the shifted call, which keeps it below high, but a money-sending path must not concatenate unvalidated attacker input.
- **Fix:** Validate every `payoutAddress` via Core `validateaddress`/`getaddressinfo` (reject invalid / wrong-network) — at registration in `upsertNode` and again at pay time. Build RPC params with a serializer or apply the existing `jsonEscape()` (already used at PoolServer.cpp:734) to `address`.

---

## 3. Should-Fix Soon (Medium)

| # | Location | Issue | Fix |
|---|---|---|---|
| M1 | `pool/PoolServer.cpp:779` (handleKeepalive/handleList) | **Payout-address takeover:** node upsert keyed only on attacker-supplied `peerId`; the client `secret` is never read. Re-POSTing a victim's peerId (read from public `/nodes.json`) overwrites its stored `payoutAddress`. Latent today (payouts off), **hard prerequisite before enabling Phase 3**. | Bind `peerId→secret` on first registration; reject payoutAddress changes lacking the matching secret. |
| M2 | `pool/PoolVerifier.cpp:153` (& :185) | **Proof-of-hosting bypass via substring match:** `peer.find(id) || id.find(peer)` against unvalidated attacker `peerId`. Register a peerId embedding a real DHT provider ID → coverage ≈ 1.0 and verification passes without hosting anything. Defeats the anti-cheat gating payouts. Escalates to high when Phase 3 is enabled. | Extract `/p2p/<id>` and compare by exact normalized base58 equality; validate registered peerIds against libp2p ID format. |
| M3 | `pool/PoolServer.cpp:646` (handleStats) | **Network I/O under `_statsMutex`:** holds the lock across blocking Core RPC (8 s) + explorer (8 s) + ip-api (12 s) calls; the 8-thread pool also serves `/permanent`, `/keepalive`, `/list`. A flood of `/pool/stats.json` captures the 30 s refresh window and stalls node endpoints. Bounded & self-healing but avoidable. | Do the calls outside the lock (copy config out, re-acquire only to store results); or refresh on a background timer; add `try_lock` serving last-known-good cache. |
| M4 | `src/Database.cpp:2606` (addIPFSJobPromise) | **Promise captured by reference across a by-value return:** correctness depends on NRVO (which MSVC omits in Debug). Without it, the stored callback dereferences a destroyed promise (worker-thread UB) and the caller's future never completes (ChainAnalyzer hang). Release builds work, so it's fragile-but-functional. | Use `make_shared<promise<string>>`, capture the shared_ptr by value, return `->get_future()`; make lifetime independent of the stack frame. |
| M5 | `src/Database.cpp:2443` (`_ipfsCallbacks`) | **Data race:** static `std::map` mutated/read across up to 10 IPFS worker threads under *three different* mutexes (or none — `getIPFSCallback` at :2340 takes no lock). Concurrent `erase` vs `operator[]`/`find` on a red-black tree → corruption/crash. | Guard every access with one dedicated mutex; copy the `std::function` out under the lock rather than returning a reference into the map. |
| M6 | `pool/PoolDatabase.cpp:361` (recordPayout) | **Successful on-chain payout with a failed ledger insert is swallowed:** returns silently on prepare failure, ignores `sqlite3_step`. DGB left the wallet but no ledger row and no log; `getLastPayoutAt()` stays stale so the once-per-period guard may not block a re-pay → double-pay. | Check `sqlite3_step == SQLITE_DONE`; on any failure emit a loud persistent ERROR with address/amount/txid; write a persistent payout log independent of the in-memory dashboard. |
| M7 | `pool/PoolDashboard.cpp:299` | **Non-idempotent payout:** a `sendtoaddress` RPC timeout after broadcast is recorded as `failed` (not ledgered); a re-run can pay the same address twice (guard may have advanced from other sends in the batch). | Write a `pending` ledger row (paidTxid NULL) before sending; on ambiguous/timeout, surface for manual reconciliation (`listtransactions`) instead of silently dropping. |
| M8 | `pool/main.cpp:228` (firstRunSnapshot) | **Partial snapshot marked complete:** on a mid-walk HTTP error it still sets `snapshotCompleted=1` and logs "Snapshot complete"; re-run is gated only on `hasPermanentData()` (any row), so a truncated import never retries → pool serves an incomplete permanent list forever, and clients under-pin content. | Track clean-end vs error termination; on error log an elevated WARNING and do **not** set `snapshotCompleted=1`; gate re-run on `snapshotCompleted` not row count. |

---

## 4. Nice-to-Have / Low

**Asset-RPC robustness (localhost + authenticated — operator footguns, not remote):**
- `src/RPC/Methods/algostats.cpp:68` & `addressstats.cpp:74` — `timeFrame=0` → integer division-by-zero in `getStatsEndTime` (Database.cpp:3113), an uncatchable hardware fault that kills the whole process. Reject `timeFrame < 1` at parse time; add a defensive guard in `getStatsEndTime`.
- `src/RPC/Methods/send.cpp:34` — unchecked JSON iterator deref (`it.name()` with no `!= end()` check) crashes the worker on `send [[{}]]` / `send [[5]]`. Validate each output is a non-empty object (sendmany.cpp:32 already does this).
- `src/RPC/Server.cpp:362` — no read timeout before auth → slowloris parks the 16-thread pool. Add a socket read deadline; prefer binding 14024 to loopback.
- `src/RPC/Server.cpp:387` — no `Content-Length` cap on the (authenticated) body read → self-DoS memory exhaustion. Cap and abort past a few MB.
- `src/RPC/Server.cpp:448` — non-constant-time credential compare (`==`). Minor given the exposed bind; the real fix is #7. Use a constant-time hash compare.

**Web server:**
- `src/WebServer.cpp:134` — binds 0.0.0.0, single-threaded synchronous accept loop with no read timeout; one stalled connection hangs the whole UI. Serves `src/` (already public). Bind 127.0.0.1, add a read timeout, serve concurrently.

**Client:**
- `cli/main.cpp:43` — `front()`/`back()` on a possibly-empty arg is UB; crashes only the short-lived CLI. Guard with `!argStr.empty()`.

**Correctness / hygiene:**
- `src/ChainAnalyzer.cpp:218` — `if (height - _pruneAge < 0)` is dead code (unsigned); returns wrapped ~4.29e9 when `height < _pruneAge`, wiping history if an operator sets `pruneage` above the tip. Use `if (height <= (unsigned)_pruneAge) return 0;`.

**Performance (all local, no attacker surface):**
- `src/RPC/Methods/listunspent.cpp:203` — O(n²) full re-sum per UTXO even when `minimumSumAmount` unset. Maintain a running sum; only compare when finite.
- `src/RPC/Methods/listassets.cpp:96` & `listlastassets.cpp:106` — per-asset N+1 DB lookups, default `numberOfRecords = UINT_MAX`. Push into one joined query; bound the default. (Cached 5760 blocks; localhost/operator-only.)
- `src/ChainAnalyzer.cpp:514` — tip-follow path uses non-verbose `getBlock()` (empty `_txCache`) → one `getrawtransaction` RPC per tx (N+1). Change condition to `if (needsAssetProcessing)` so tip blocks also fetch verbose.
- `src/Database.cpp:916` — `getAsset()` re-reads asset row + KYC row + deserializes rules on every asset-bearing input. Add a bounded LRU keyed on `assetIndex`, invalidated on `addAsset`.
- `src/Database.cpp:277` — prune `DELETE FROM utxos WHERE heightDestroyed<?` full-scans (no usable index); amplified under `storenonassetutxo=true`. Add partial index `ON utxos(heightDestroyed) WHERE heightDestroyed IS NOT NULL`.
- `pool/PoolDatabase.cpp:482` — `getSampleCids` uses `ORDER BY RANDOM()` (full scan+sort) every 60 s. Sample by random rowid boundary.
- `pool/PoolDashboard.cpp:483` — render() runs 8 DB counts (incl. `COUNT(DISTINCT assetId)` full-scan) every 500 ms, contending `_mutex` with `/permanent`. Cache near-static counts, refresh at 5–30 s.
- `src/ConsoleDashboard.cpp:466` — synchronous `getBlockCount()` RPC on the 500 ms render thread. Cache the tip, refresh every 2–5 s.

**Input validation / robustness (pool, cosmetic impact):**
- `pool/PoolServer.cpp:291` & `:310` (merged) — **X-Forwarded-For spoofing:** left-most XFF token trusted; `isPublicIp()` does no syntax validation, so arbitrary strings (incl. quotes) are stored as `lastAddr` and interpolated **unescaped** into the ip-api batch body. A single `X-Forwarded-For: 8.8.8.8"x` makes every batch malformed → no *new* node ever geolocates (persistent, degrades only the decorative world map; no XSS — output is jsonEscaped/numeric). Also unbounded unauthenticated node-row creation (disk-fill). Fix: `inet_pton`-validate before storing, `jsonEscape` the ip-api body, trust XFF only from the known proxy hop, rate-limit registration in Caddy.

**Secret hygiene / observability:**
- `src/DigiByteCore.cpp:118` — `rpcpassword` printed in cleartext to stderr when `DGBCORE_DEBUG_URL` set (opt-in, off by default). Redact userinfo before logging.
- `src/PermanentStoragePool/pools/mctrivia.cpp:545` — per-node `secret` logged at DEBUG. (File-logging path is dead code, so exposure is limited to the operator's console/stdout.) Redact to `&secret=<redacted>`.
- `src/DigiByteCore.cpp:158` — `errorCheckAPI` discards RPC code/message; every Core error surfaces as "Core Offline", misleading sync diagnosis. Log real code+message; throw `exception(e.what())`.
- `src/Database.cpp:1624` — `catch(...){}` swallows a `listUnspent` fallback failure on pruned nodes → under-reported balances with no log. Log the failure with address + error.
- `pool/PoolServer.cpp:779` (handleKeepalive/handleList) — `upsertNode` wrapped in empty `catch(...)`; DB write failure is swallowed and the client is still told OK → registrations silently dropped, no trace (PoolServer has no logger). Log to stderr; thread a logger into PoolServer.
- `src/IPFS.cpp:167` — terminal (final-retry) IPFS download failure drops the CID with no log identifying it. Emit a WARNING/DEBUG line with the CID.
- `src/PermanentStoragePool/pools/mctrivia.cpp:658` — bad-list load failure logged at DEBUG without `e.what()`; stale content-takedown feed goes unnoticed. Include the error, escalate to WARNING after N consecutive failures.
- `pool/deploy/setup-caddy.ps1:59` — `caddy.exe` downloaded with no hash/signature check, then run as SYSTEM at boot (inconsistent with the SHA-verified kubo/DigiByte downloads). Add `Get-AuthenticodeSignature`-Valid check (or pinned GitHub SHA-512) before writing/registering.
- `setup-digiasset.ps1:693` (also `:713`, `:890`, `setup-pool.ps1:101`, `node/update-binaries.ps1:92`) — project's own exes + self-update script fetched from GitHub releases and auto-run as SYSTEM every 6 h with no signature verification. Authenticode-sign artifacts and verify before launch; use a detached signature (not a co-hosted SHA) for the self-updating script; enforce GitHub 2FA/release protection.

---

## 5. Doc ↔ Code Consistency Issues

_All documentation-only; no runtime/security impact. Grouped separately._

**Payout mechanics (docs understate the implemented weighting):**
- `pool/README.md:216` — says payouts are a "flat split … no weighting." Code weights by `coverage × reliability` (PoolDatabase.cpp:345; PoolDashboard.cpp:338/405). Update to match FAIRNESS.md.
- `pool/README.md:103–104` — pool.cfg table says budget is "split equally across verified nodes." Actually proportional to weight. Reword to "in proportion to their coverage × reliability weight."
- `pool/README.md:159` & `POOL-SETUP.md:143` — payout-eligibility list omits the `coverageScore > 0` gate the code enforces (PoolDatabase.cpp:331/346). Add it (FAIRNESS.md:29 already lists it).
- `ARCHITECTURE.md:196` — says automated payouts are a "future phase … no DGB moves." The `[P]/[E]` weighted `sendtoaddress` flow is fully implemented, gated behind `poolpayouts=1`. Rewrite; also reconcile the stale "Phase 3 … not shipped" runtime message at `pool/main.cpp:271`.

**Web server exposure (docs claim local-only; code binds 0.0.0.0):**
- `example.cfg:113` (& `ARCHITECTURE.md:143`) — calls the web server "optional … only runs if you launch the web app." It's built in and always started (main.cpp:333). Update.
- `NODE-SETUP.md:97` — calls port 8090 "local-only," but `WebServer.cpp:134` binds all interfaces (and the installer's `-Profile Any` firewall rule makes it LAN-reachable). Either bind 127.0.0.1 (recommended — see §4) or document that it binds all interfaces.

**Route/version drift:**
- `readme.md:206` — manual-install points at DigiByte v8.22.2; installer pins v9.26.4 (`setup-digiasset.ps1:51`). Update to 9.26.x or link "latest."
- `pool/deploy/README.md:9` — proxied-route list omits `/pool/stats.json`, which the landing page depends on and `Caddyfile:23` includes. Add it (a hand-transcribed proxy would break the stats/map page).
- `ARCHITECTURE.md:186` — documents `GET/POST /list/<floor>.json`; server routes `POST /list/` only (PoolServer.cpp:579), GET falls to 404. Drop the `GET/`. (Cited line 188 is inaccurate; the text is on 186.)

---

_End of report._
