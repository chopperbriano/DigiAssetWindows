# Transaction Encoding & Database Layer — Cheat Sheet

For Fable picking up Task 1 (create/issue assets) and Task 2 (send assets) on the
`last_tasks` branch. **Good news: the on-wire DigiAsset protocol is already fully
documented in doc-comments in the decode-side code.** You do not need to reverse-engineer
it from a spec doc — read the comments cited below, they ARE the spec.

---

## 1. `src/DigiByteTransaction.h` / `.cpp` — the transaction object

Constants (top of class, `src/DigiByteTransaction.h:20-27`):
```cpp
STANDARD = 0, DIGIASSET_ISSUANCE = 1, DIGIASSET_TRANSFER = 2, DIGIASSET_BURN = 3,
KYC_ISSUANCE = 10, KYC_REVOKE = 11, EXCHANGE_PUBLISH = 20, ENCRYPTED_KEY = 30
```

Two constructors:
- `DigiByteTransaction()` (`.cpp:23`) — blank, for building a NEW tx (this is what
  issuance/send code should start from).
- `DigiByteTransaction(txid, height, dontBotherIfNotSpecial)` (`.cpp:32`) — loads and
  decodes an EXISTING on-chain tx. This is the decode path; read it to understand
  ordering (`decodeKYC` → `decodeExchangeRate` → `decodeEncryptedKeyTx` → `decodeAssetTX`
  → `storeUnknown`, see `.cpp:118-122`).

**The two methods Fable needs to implement, both in `src/DigiByteTransaction.h:99-100`:**
```cpp
void addDigiByteOutput(const std::string& address, uint64_t amount);
void addDigiAssetOutput(const std::string& address, const std::vector<DigiAsset>& assets);
```
- `addDigiByteOutput` **has a stub body** at `src/DigiByteTransaction.cpp:840-844` —
  three `//todo` comments, no code:
  ```cpp
  void DigiByteTransaction::addDigiByteOutput(const string& address, uint64_t amount) {
      //todo check its unlocked
      //todo check there is enough funds to send(and still pay tx fee)
      //todo add the output
  }
  ```
  It's already called once, from `src/PermanentStoragePool/pools/mctrivia.cpp:110`
  (`tx.addDigiByteOutput(outputAddress, cost);`) — so whatever signature/behavior you
  build must stay compatible with that call site.
- `addDigiAssetOutput` **has ZERO implementation anywhere** — confirmed via
  `grep -rn addDigiAssetOutput src/ tests/ cli/ qt/` → only the header declaration and
  this doc file reference it. This is greenfield.
- There IS an existing private helper `addAssetToOutput(size_t output, const DigiAsset&
  asset)` (`.cpp:559`) used internally by the decode path to push an asset onto
  `_outputs[output].assets` — useful/reusable when building `addDigiAssetOutput`, but it
  operates on the in-memory `_outputs` vector only; it doesn't touch the on-chain
  encoding (OP_RETURN bytes). The OP_RETURN encoding side is what's actually missing.

Other relevant methods already implemented and usable as-is:
- `addToDatabase()` (`.cpp:577`), `lookupAssetIndexes()` (`.cpp:670`) — persistence,
  probably needed after a new asset issuance tx confirms.
- `toJSON()` (`.cpp:~700-825`) — shows the full output shape used elsewhere; good
  reference for what fields matter (kyc, exchange, asset outputs, etc).
- `checkRulesPass()` (`.cpp:456`) — validates asset rules (royalties/approval/geofence)
  are satisfied by a transfer; only used on decode currently, but the same logic will
  matter if you want to pre-validate a send before broadcasting.

No CScript/OP_RETURN-byte-building helpers exist yet inside `DigiByteTransaction` itself
— but **all the low-level bit-packing primitives you need already exist in `BitIO`**
(`src/BitIO.h`), see §2 below. This is a write-side gap in the transaction-assembly
layer, not a missing bit-level toolkit.

---

## 2. `BitIO` (`src/BitIO.h`) — the encoding primitives you'll actually use

`BitIO` is bidirectional: every decode primitive used in `decodeAssetTX` /
`decodeAssetTransfer` has a matching encode primitive already implemented:

| Read (decode, already used)      | Write (encode, ready to use)                    |
|-----------------------------------|--------------------------------------------------|
| `getBits(length)`                 | `setBits(value,length)` / `appendBits(value,length)` / `insertBits(...)` |
| `getFixedPrecision()`             | `static makeFixedPrecision(uint64_t value)`       |
| `getDouble()`                     | `static makeDouble(double value, bool isLE=true)` |
| `getBitcoinData()` / `checkIsBitcoinOpReturn()` | `static makeBitcoin(...)` (builds OP_RETURN wrapper, `includeOpReturn` flag) |
| `getXBit(xLength)`                | `static makeXBit(const xBitValue&)`               |
| `getHexString`/`getUTF8String`/`getAlphaString`/`get3B40String` | matching `make*String` statics |

So building the OP_RETURN payload for an issuance/transfer is mechanically: construct a
`BitIO`, `appendBits`/`setBits` the header+body fields in the exact order the decode
functions read them (see §3), then wrap with `BitIO::makeBitcoin(data, true)` to get a
proper OP_RETURN script, and that hex becomes a `vout.scriptPubKey.hex`-equivalent output
to feed into `createrawtransaction` (see §4).

---

## 3. The wire format itself (already spec'd in comments — just follow it)

### Header — `src/DigiAsset.cpp:220-247` (`DigiAsset::decodeAssetTxHeader`)
```
byte 0-1: 0x4441  ("DA" magic)
byte 2:   version number (0 not allowed; current is version 3)
byte 3:   opcode
```
Opcode meanings, quoted directly from the doc comment (`DigiAsset.cpp:234-241`):
```
0x01 - Issuance. No rules
0x02 - Issuance. No rules, data in multisig (sunset, don't use — V3 doesn't need it)
0x03 - Issuance. Rules that can be changed
0x04 - Issuance. Rules that can not be changed
0x05 - Issuance. No MetaData or rules (not recommended)
0x15 - Transfer   (opcode >=16 and <48, (opcode%16)==5, <0x20 = transfer)
0x25 - Burn       (same range, opcode >=0x20 = burn; sending to output 31 with no range/percent bits = full burn)
```

### Issuance body — `src/DigiAsset.cpp:287-303` (`DigiAsset::processIssuance` doc comment)
Quoted verbatim — this is the authoritative spec:
```
if (version <3 and opcode <3) skip next 20 bytes (unused legacy field)
if (opcode 1,3, or 4) next 32 bytes = sha256 of the metadata (IPFS content hash → CID)
next 1-8 bytes: number of assets to create (BitIO::getFixedPrecision / makeFixedPrecision format)
next N bytes: rules, if any (see DigiAssetRules — §3a below)
next N bytes: transfer instructions (see decodeAssetTransfer — §3b below)
last 1 byte: issuance flags (bit 0 = LSB)
    bits 7,6,5: divisibility (0-8)
    bit 4: locked
    bits 3,2: aggregation (0=aggregable,1=dispersed,2=hybrid — see DigiAsset.h constants)
```
Opcode 2 (sunset, avoid for new code) pulls the metadata hash out of a multisig output
instead of inline (`DigiAsset.cpp:326-332`) — don't replicate this for new issuance code.

Divisibility/locked/aggregation are also derivable from the human-readable `assetId`
Base58 string itself for existing assets — see `DigiAsset.cpp:191-208` — but for a NEW
issuance you'll be computing the flags byte directly, then `calculateAssetId()` derives
the assetId from `txData.vin[0]` + the flags byte (`DigiAsset.cpp:371`, implementation
around line 140-156).

### 3a. Rules encoding — `src/DigiAssetRules.cpp:17-35` doc comment
```
Rules data is always a multiple of 8 bits, commands are NOT byte-aligned.
After the last rule, four 1-bits signal "rules done", then pad remainder of byte with 1s.
Each rule = 4-bit header nibble + variable-length body:
0x0 Approval        0x1 Royalties         0x2 Geofence Allowed   0x3 Geofence Denied
0x4 Vote/expiry      0x5 Required burn      0x9 Royalty price unit   0xf Rules done
```
Sub-formats for each rule type are documented inline further down in
`DigiAssetRules.cpp` (e.g. Approval encoding detailed at lines 84-93+) — read the
decoder for whichever rule type you need to support for issuance.

### 3b. Transfer instruction encoding — `src/DigiByteTransaction.cpp:289-298` doc comment
```
dataStream positioned at transfer section.
Loop while bits-left > footerBitCount (8 bits reserved if issuance, else 0):
  1 bit:  skip
  1 bit:  range
  1 bit:  percent
  5 or 13 bits: output index (13 bits if range=1, else 5 bits)
  amount: if percent=1 → 1 byte (0xff=100%, 0x00≈0.39%, formula: input_count*(byte+1)/256)
          else → BitIO::getFixedPrecision() (variable length)
  if range=1: totalAmount = (output+1) * amount   [applies to outputs 0..output]
  else:       totalAmount = amount                [applies to single `output`]
```
Full consuming logic (which input asset gets debited, how leftover becomes "change" on
the last output) is at `DigiByteTransaction.cpp:298-450` — read this closely since a
send/transfer builder must replicate the SAME instruction semantics the decoder expects,
or round-tripping (issue → later decode by any node) will fail asset-id validation.

**Practical implication for Task 2 (send assets):** you don't need to implement the full
generality (skip/range/percent) on day one — a minimal "send N of asset X from these
inputs to this one output, rest to change output" encoder only needs range=0, percent=0,
skip=0 for each instruction, which is the simplest legal encoding.

---

## 4. Database layer (`src/Database.h`, 606 lines)

Full schema/PRAGMA/table summary already exists in `/Users/mc/Desktop/DigiAsset_Core/DATABASE_OPTIMIZATION_PLAN.md`
(root of repo) — read that instead of re-deriving schema info here.

Balance/holdings-relevant methods, all in `src/Database.h`:

| Method | Line | What it does |
|---|---|---|
| `getAddressHoldings(address)` → `vector<AssetCount>` | 463 | **THE method Task 3's `getaddressholdings` RPC uses.** Returns `{assetIndex, count}` pairs for ONE address. `AssetCount` struct (line 69-72) has only `assetIndex` + `count` — **no name, no decimals, no assetId string.** |
| `getAddressUTXOs(address, minConfirms, maxConfirms)` → `vector<AssetUTXO>` | 460 | Lower-level: raw UTXOs (with embedded assets) for an address. |
| `getAssetUTXO(txid, vout, height)` | 448 | Assets sitting on one specific UTXO — used when building tx inputs. |
| `getAssetHolders(assetIndex \| assetId)` | 449-450 | Reverse lookup: who holds a given asset. |
| `getTotalAssetCount` / `getOriginalAssetCount` | 451-454 | Supply figures. |
| `getRules(assetId)` | 402 | Current rules for an asset (needed to validate a transfer before building it — see `checkRulesPass`). |
| `getSendingAddress(txid, vout)` | 444 | Resolves the address that funded an input — used for issuer/KYC lookups. |
| `getAddressKYC(address)` | 438 | KYC record for an address. |
| `isWatchAddress(address)` | 420 | Exchange-rate watch address check. |

**Confirmed gap for Task 3:** there is no wallet-wide balance aggregator. The existing
`getaddressholdings` RPC (`src/RPC/Methods/getaddressholdings.cpp`, full file — it's
short, ~40 lines) is a thin wrapper: 1 param (address) → `db->getAddressHoldings()` →
JSON `{assetIndex: count}`. Use this file as the **exact template** for a new
`getbalance`-style RPC (see `fable_cheat_sheet/02_rpc_cli_and_build.md` for how new RPC
methods get registered).

To build a real wallet-wide balance RPC you'd need to:
1. Enumerate wallet addresses — DigiByteCore wrapper has `getaddressesbylabel`,
   `listlabels`, `listaddressgroupings`, or `listunspent` (all in `src/DigiByteCore.h`,
   see `fable_cheat_sheet/02_rpc_cli_and_build.md` §4/§ wallet notes) — pick whichever
   gives full wallet address coverage including empty/change addresses.
2. Call `db->getAddressHoldings(addr)` per address, aggregate `assetIndex → count` across
   all of them.
3. For each distinct `assetIndex`, resolve name/decimals/assetId by constructing a
   `DigiAsset` — see `getassetdata.cpp` RPC (`src/RPC/Methods/getassetdata.cpp`) for the
   exact lookup pattern (it accepts either `assetIndex` or `assetId`+`txid`+`vout`, and
   calls `DigiAsset::toJSON()` for the output shape — reuse that JSON shape for
   consistency).

---

## 5. DigiByte Core wallet RPC integration (`src/DigiByteCore.h`, 259 lines)

This class is the wrapper around the DigiByte Core JSON-RPC wallet API. Everything
needed to build+sign+broadcast a raw asset tx **already has typed wrappers** except one:

| Wrapper | Line |
|---|---|
| `listunspent(minconf, maxconf, addresses)` | 180 |
| `listlockunspent()` | 181 |
| `lockunspent(unlock, outputs)` | 182 — **this is the UTXO-locking mechanism** the prior notes flagged as needed to keep `fundrawtransaction` from spending asset-bearing UTXOs as fee inputs. It's already wired up as a typed call; nothing new needed here, just USE it: lock every asset-bearing UTXO before calling fundrawtransaction, unlock after. |
| `createrawtransaction(inputs, amounts)` (2 overloads) | 209, 211 |
| `signrawtransaction(rawTx, ...)` (2 overloads) | 213, 215 |
| `sendrawtransaction(hexString, highFee)` | 207 |
| `getrawchangeaddress()` | 220 |
| `getnewaddress(label, type)` | 130 |

**`fundrawtransaction` has NO typed wrapper** — grep confirms it's absent from
`DigiByteCore.h`'s method list entirely. Two options: (a) add a typed wrapper following
the existing pattern (look at `sendrawtransaction`'s implementation in
`DigiByteCore.cpp` as a template for how params/response get marshalled), or (b) use the
existing generic passthrough `Json::Value sendcommand(const std::string& command, const
Json::Value& params)` (`DigiByteCore.h:101`) to call it untyped — this already exists
and requires zero new code, just less type safety. Given the RPC-wallet layer is a thin
JSON-RPC client either way, (b) is the faster path if Fable is time-constrained.

All wallet RPC calls go out over the JSON-RPC connection configured via `setFileName` /
`makeConnection` (`DigiByteCore.h:80,86`) — this is the same connection used for
blockchain queries, just needs the wallet-enabled DigiByte Core node (now true — the
Mac's `digibyted` was rebuilt with `--enable-wallet --without-bdb --with-sqlite=yes`
during this session, see project memory / prior conversation turns).

---

## Summary: what's genuinely missing vs. what's just plumbing

**Missing (must write):**
- `addDigiByteOutput` body (trivial — output construction + fee/balance checks)
- `addDigiAssetOutput` body (the real work — OP_RETURN encoding per §3, using BitIO
  primitives from §2)
- A wallet-wide balance RPC (aggregation logic, §4 — but the template RPC file and all
  DB getters already exist)
- Possibly a `fundrawtransaction` typed wrapper (or just use `sendcommand` passthrough)
- CLI/GUI surfaces for whatever new RPC methods get added (trivial — see
  `fable_cheat_sheet/02_rpc_cli_and_build.md`, CLI is a generic passthrough already)

**NOT missing (reuse, don't rebuild):**
- Bit-level encode/decode primitives (`BitIO`)
- The entire wire-format spec (documented in decode-side doc comments, cited above)
- UTXO locking (`lockunspent`)
- Raw tx create/sign/broadcast wrappers
- All the DB read methods needed to source balances/UTXOs/rules
