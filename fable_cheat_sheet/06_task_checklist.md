# Task Checklist — 5 Tasks Mapped to Files/Functions

Cross-reference between `LAST_TASKS_NOTES.md`'s 5-task table and the concrete files/functions
found across `02`–`05`. Read the linked topic file for full detail — this is just the map.

**Read `07_corrections_and_gotchas.md` before trusting any prior-session claim below or in
`LAST_TASKS_NOTES.md`/`TODO_TESTS.md` — several turned out to be stale.**

---

## Task 1 — Create assets via CLI + GUI

**CLI side:** no new CLI code needed — `cli/` is a generic passthrough (`02_rpc_cli_and_build.md`
§4). Once an RPC method like `createasset` exists, it's automatically CLI-available.

**Core work — new RPC method + encoder:**
- Add `src/RPC/Methods/createasset.cpp` (or similar name) — filename becomes the method name,
  auto-registered via CMake glob, no MethodList.cpp edit needed (`02_rpc_cli_and_build.md` §2).
  Use `src/RPC/Methods/version.cpp` as the minimal template, `getassetdata.cpp` (89 lines) as a
  richer template for building/returning `DigiAsset` JSON.
- The actual encoding logic belongs in `DigiByteTransaction` (`03_transaction_encoding_and_database.md`
  §1): `addDigiAssetOutput` is declared but **has zero implementation anywhere** (grepped whole
  repo). This is the core missing piece. `addDigiByteOutput` (`DigiByteTransaction.cpp:840`) is
  also just a 3-line todo stub and will likely be needed too (asset txs still need a DGB change
  output).
- **The wire format is NOT a reverse-engineering job** — `03_transaction_encoding_and_database.md`
  §2 shows the DigiAsset v3 issuance/transfer format is fully documented in doc-comments on
  `DigiAsset.cpp`, `DigiAssetRules.cpp`, and the decode functions in `DigiByteTransaction.cpp`.
  `BitIO` (`src/BitIO.h/.cpp`) already has matching write-side primitives (`setBits`,
  `makeFixedPrecision`, `makeBitcoin`, etc.) for every read primitive the decoder uses — read
  the decode side, write the mirror using these existing helpers.
- Wallet interaction: `DigiByteCore.h` already wraps `createrawtransaction`, `signrawtransaction`,
  `sendrawtransaction`, `lockunspent` (`03_...md` §4). No `fundrawtransaction` wrapper exists yet
  — either add one, or use the generic `sendcommand()` passthrough already on `DigiByteCore` as a
  zero-code fallback.

**GUI side:** new "Create Asset" tab in `qt/tabs/`, follow `SyncTab` pattern exactly
(`04_qt_gui_and_web_docs.md` §1-2). **Remember:** unlike RPC methods, Qt tabs are NOT
auto-registered — must manually add the tab class to `qt/CMakeLists.txt` AND wire it into
`qt/main.cpp`'s tab-mounting code (exact insertion point at `main.cpp:148-157` per the GUI fork's
findings).

---

## Task 2 — Send assets via CLI + GUI

Same encoder work as Task 1 applies (`addDigiAssetOutput` for `DIGIASSET_TRANSFER` instead of
`DIGIASSET_ISSUANCE` — see the tx-type constants in `03_...md` §1). Additional considerations:

- Existing `send`/`sendtoaddress`/`sendmany` RPCs (`02_rpc_cli_and_build.md` §1) are DGB-only
  domain-resolution passthroughs — NOT a starting point for asset sends, don't extend them; write
  a new method (e.g. `sendasset`).
- **UTXO safety:** prior notes flagged that `fundrawtransaction` must not consume asset-bearing
  UTXOs as fee inputs. `lockunspent` is already wrapped in `DigiByteCore.h` — use it to lock asset
  UTXOs before calling `fundrawtransaction`, or build the raw tx with explicit inputs and skip
  `fundrawtransaction` for the asset input entirely (manually add a DGB-only input for fees).
- GUI: new "Send Asset" tab, same registration gotcha as Task 1.

---

## Task 3 — Balance including assets via CLI + GUI

- `getaddressholdings <address>` (`getaddressholdings.cpp`, `02_...md` §1 and `03_...md` §3)
  already exists and is the closest template — but it's single-address, no wallet-wide
  aggregation, no asset name/decimals resolution (raw `{assetIndex: qty}` only).
- Database layer already has the needed getters (`getAddressHoldings`, asset UTXO queries, rules)
  per `03_transaction_encoding_and_database.md` §3 — the gap is a wallet-wide aggregator RPC that
  loops over all wallet addresses (via `listunspent`/`listaddressgroupings` from the DigiByte
  wallet) and sums holdings per asset, then resolves names/decimals via `Database::getAsset`.
- GUI: new "Balances" tab, same `SyncTab`-pattern + manual-registration requirement.

---

## Task 4 — Finish test suite (all code tested)

**Do this FIRST, before writing any new tests:** `TODO_TESTS.md`'s bug table (5-8) is stale —
those bugs are already fixed and committed, and `tests/PermanentStoragePoolTest.cpp` (which
TODO_TESTS.md says doesn't exist) already exists with 4 tests. Full verification in
`05_tests_and_psp.md` §"CRITICAL". Update or delete `TODO_TESTS.md` once confirmed so this
doesn't cost a future session time again.

**Actual remaining gap list** (`05_tests_and_psp.md` §"Gap list"):
- Source files with no test file: `AppMain.cpp`, `DigiByteDomain.cpp`, `OldStream.cpp`,
  `PermanentStoragePoolList.cpp`, `PermanentStoragePool/pools/local.cpp`, `RPC/Server.cpp`,
  possibly `Database_LockedStatement.cpp` (verify — may be covered inside
  `Database_StatementTest.cpp`).
- RPC method with no test: `getrandom.cpp` has no `tests/RPC_Methods/getrandom.cpp` — this one
  IS auto-globbed by CMake (no CMakeLists.txt edit needed), unlike the source-file gaps above
  which need manual `set(TESTS ...)` edits in `tests/CMakeLists.txt` (`05_...md` §"CMake Test
  Target" for the exact mechanism/line numbers).
- **`tests/testFiles/` is not a static fixture** — the real test DB is downloaded from IPFS at
  runtime by `DigiAssetTransactionTest.cpp`, and (surprisingly) only needs `digibyted`'s RPC port
  reachable, NOT a fully synced chain (`05_...md` §"testFiles"). Worth trying now even mid-resync
  — don't assume you're blocked for a week.

---

## Task 5 — Documentation: web docs + readme (all OS, remove "without wallet" note)

- Stale wallet-support note: `web/index.html:558-563` (confirmed exact lines,
  `04_qt_gui_and_web_docs.md` §2) — remove/reword, wallet build is required now that the daemon
  is being rebuilt with wallet support.
- RPC doc coverage: all 35 `.cpp` methods have matching `.html` docs, EXCEPT `getrandom` has a doc
  file that exists but **no nav link** in `web/index.html` — a real, fixable gap
  (`04_...md` §2).
- `readme.md` issues (exact lines quoted in `04_...md` §3): `wget wget` typo (line 56), version
  references to say v8.22.x not v7.17.3, systemd path mismatch, no macOS/Windows sections.
- **Windows section is blocked on a branch decision, not just writing:** Windows/vcpkg compat
  work already exists (commit `47568e1`) but only on the unmerged `fast` branch. Writing Windows
  build instructions into `readme.md` on `last_tasks` without merging that work first would
  describe an unsupported build — flag this to the user before writing that section.

---

## Suggested order of attack

1. Skim `07_corrections_and_gotchas.md` (2 min) so you don't redo finished work.
2. Task 4's easy wins first (the `getrandom` test + `AppMain`/`Server.cpp`/etc. gap tests) —
   no chain/IPFS dependency, pure unit tests, low risk.
3. Task 1/2 encoder work (`addDigiAssetOutput`) — the meaty, novel part; everything else (RPC
   method, CLI, GUI tab) is mechanical once the encoder exists and can reuse the same function
   for both issuance and transfer.
4. Task 3 balance aggregation RPC — straightforward once you've seen the encoder's UTXO-handling
   code, since both need to enumerate wallet UTXOs.
5. Task 5 docs — cheap, do last or interleave; the wallet-note removal in particular can be done
   any time since it doesn't depend on other tasks.
