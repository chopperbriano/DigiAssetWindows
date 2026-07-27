# Scope: Rule-Aware Asset Transfers

**Status:** proposed · **Owner:** TBD · **Related release:** win.109 added the interim *reject* guard

## Problem

DigiAsset rules (royalty, deflation, signers, expiry, vote, KYC) are enforced by
every indexer on **replay** via `DigiAsset::checkRulesPass()`
([`src/DigiAsset.cpp:722`](../src/DigiAsset.cpp)). A transfer that does not satisfy a
rule throws `exceptionRuleFailed`; the decoder
([`src/DigiByteTransaction.cpp:282`](../src/DigiByteTransaction.cpp)) catches it,
clears the asset outputs, and sets `_unintentionalBurn` — i.e. **the asset is
destroyed**.

Today only `issueasset` ever calls `tx.addRuleOutputs()`
([`src/RPC/Methods/issueasset.cpp:383`](../src/RPC/Methods/issueasset.cpp)), and
`addRuleOutputs()` itself throws unless the tx is an issuance
([`src/DigiByteTransaction.cpp:930`](../src/DigiByteTransaction.cpp)). The transfer
methods (`sendasset`, `sendmanyassets`, `burnasset`) build only
`buildTransferInstructions` and add **no** rule outputs. So transferring a
rule-bound asset silently burns it.

**Interim mitigation (shipped in win.109):** `AssetWallet::assertTransferableAsset`
rejects `sendasset`/`sendmanyassets`/`burnasset` for assets that require a
**royalty** payment or a **deflation** burn — the two rules that *deterministically*
burn on any transfer. This converts silent loss into a clear error but does **not**
let those assets move.

## Goal

Let a wallet transfer any rule-bound asset by constructing a transaction that
satisfies the asset's rules, so `checkRulesPass()` passes on replay. Remove the
interim reject guard once each rule class is supported.

## Rule-by-rule work

| Rule | Trigger (`checkRulesPass`) | What a compliant transfer must add | Difficulty |
|------|----------------------------|-------------------------------------|------------|
| **Royalty** | `getIfRequiresRoyalty()` → needs an output paying each royalty address ≥ `count × amount × rate / 1e8` DGB (`DigiAsset.cpp:774`) | One DGB output per royalty recipient, sized from the accepted exchange rate at the current height; `count` = number of receiving asset outputs | **High** — needs the exchange-rate lookup the indexer uses (`getAcceptedExchangeRate`) and correct recipient counting |
| **Deflation** | `getRequiredBurn() > 0` (`DigiAsset.cpp:833`) | Burn `getRequiredBurn()` units (a burn output) in addition to the transfer | **Medium** — add a burn instruction; interacts with change |
| **Signers** | `getRequiredSignerWeight() > 0` (`DigiAsset.cpp:757`) | Ensure inputs include enough signer-address weight; may require multi-party signing | **High** — coordination / PSBT-style flow if the wallet doesn't hold the signer keys |
| **Expiry** | `getIfExpired(height,time)` (`DigiAsset.cpp:822`) | Nothing to add — an expired asset genuinely can't move | **Low** — reject with a clear "asset expired" error (correct behavior) |
| **Vote** | `getIfVoteRestricted()` → each receiver must be a valid vote address (`DigiAsset.cpp:825`) | Validate the recipient address against the rule before building; reject early if invalid | **Low–Med** — pre-send validation only |
| **KYC / geofence** | `getIfGeoFenced()` → each receiver must be KYC-valid & country-allowed (`DigiAsset.cpp:807`) | Pre-check `getAddressKYC(recipient)`; reject early if not allowed | **Medium** — depends on local KYC data being present |

Note `checkRulesPass` returns early for a **consolidation** (no address gains
assets) or a **pure burn** — so a full burn of a royalty/deflation asset is
actually safe today; only partial transfers/burns (which leave change) trip the
rules. A precise implementation can therefore allow full burns even before full
royalty support lands.

## Proposed phases

1. **Phase 0 (done, win.109):** reject royalty/deflation transfers instead of
   burning.
2. **Phase 1 (done, win.110):** `assertTransferableAsset` now mirrors
   `checkRulesPass`'s full enforced set — royalty, deflation, **signer, vote,
   KYC/geofence, and expiry** — and rejects each with a specific reason, so no
   enforced transfer rule can silently burn. The guard is applied to every asset
   that lands in a *receiving* output: the sent asset(s) in `sendasset`/
   `sendmanyassets`, and any leftover/bystander **change** assets in all three
   methods. **Full burns are now allowed** — a burn with no leftover change is a
   pure burn/consolidation that `checkRulesPass` permits, so `burnasset` only
   guards when a change output would exist. *Still conservative:* vote/KYC assets
   are rejected outright rather than validating the recipient (Phase 2 refines).
3. **Phase 2 — royalty transfers:** add a `addTransferRuleOutputs()` builder that
   appends royalty payment outputs (mirror `addRuleOutputs`, but for transfers),
   wired into `sendasset`/`sendmanyassets`. Fund the royalty DGB from the wallet.
   Round-trip test against `checkRulesPass` with a captured fixture.
4. **Phase 3 — deflation transfers:** add the required burn output; reconcile with
   change and fee.
5. **Phase 4 — signer assets:** design a signing flow (single-wallet fast path
   when the wallet holds signer keys; PSBT/multi-party otherwise). Largest effort;
   may be deferred.

## Testing

- Fixtures replaying each rule class through `checkRulesPass` (pass **and** fail).
- Unit tests for the royalty amount math (count × amount × rate) and the burn math.
- An integration test that builds a transfer via the RPC method and asserts the
  resulting tx passes `DigiByteTransaction::checkRulesPass()` without
  `_unintentionalBurn`.

## Risks & decisions to confirm

- **Exchange-rate source:** royalty sizing must use the *same* accepted rate the
  indexers use at that height, or the payment is short and the asset burns. Confirm
  `getAcceptedExchangeRate` is the canonical source and is available wallet-side.
- **Reissue:** `reissueasset` also never calls `addRuleOutputs`; confirm whether a
  reissue of a ruled asset is affected (likely a consolidation to the issuer, but
  verify) and fold into Phase 2/3 if needed.
- **Scope of Phase 4 (signers):** decide whether multi-party signing is in scope or
  whether signer-restricted assets remain reject-only for now.

## Recommendation

Ship **Phase 1** next (small, removes the remaining silent-burn paths the interim
guard doesn't cover), then decide on Phase 2 (royalty) as the first "real" transfer
support based on whether the community actually issues royalty assets.
