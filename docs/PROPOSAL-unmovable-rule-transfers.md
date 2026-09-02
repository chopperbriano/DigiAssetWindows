# Proposal: refuse wallet-built transfers that would destroy the asset

**Branch:** `upstream/refuse-unmovable-rule-transfers`, based on `asset_features` @ `fb3aa1d`
**Scope:** one function and one call site in `src/AssetWallet.cpp`
**Consensus impact:** none — see [§4](#4-why-this-is-not-a-consensus-change)
**Reported by:** Ray, [Brasa Studios](https://brasastudios.games) — see [§7](#7-credit)

---

## 1. Summary

An asset issued with a **royalty**, **required-burn** or **required-signer**
rule cannot be transferred by `sendasset`. That much is expected — those rules
need outputs the transfer builder does not add.

What is not expected is the outcome. The transfer does not fail. It **destroys
the sender's entire holding of the asset**, including the units they were
keeping, returns a txid, and logs nothing.

This branch makes `sendasset` refuse to build such a transfer, with a reason
naming the rule. It changes nothing else.

---

## 2. What happens today

`DigiByteTransaction::decodeAssetTransfer` throws `exceptionRuleFailed` when an
asset's rules are not satisfied. The handler in `DigiByteTransaction.cpp`
catches it and clears every asset output in the transaction:

```cpp
} catch (const DigiAsset::exceptionRuleFailed& e) {
    //clear asset outputs
    _unintentionalBurn = true;
    for (AssetUTXO& output: _outputs) {
        output.assets.clear();
    }
    return false;
}
```

The change output is an asset output. So a transfer of 1 unit out of a holding
of 5 clears the recipient's output *and* the 4-unit change output. The sender
ends with nothing, the recipient receives nothing, and supply drops to zero.

`sendasset` has already returned success by then: the transaction is valid at
the DigiByte layer and confirms normally. Only the asset layer treats it as a
burn, and it does so silently.

### Verified on mainnet

Ray tested it deliberately with a throwaway asset rather than reporting a
suspicion:

| | |
|---|---|
| asset | `La9T71BHHeX8fSAnmqnSUkREY3ekJPGiGnhjTW` — 5 units, locked, royalty rule |
| issuance | `8adb12ad7f4b1f979a91a53475a6a9fc5610ed7541e3fa7cbadb4bf53484f73e`, block 24,106,262 |
| the send | `f385d004eca95a52b415a21d3e8c01c2a5431f41849f7de01c17dfb2a9fdfdc0`, block 24,106,276 |

He sent 1 unit. All 5 were destroyed. Both transactions are on chain and can be
checked independently of anything in this document.

### Why it matters

A collector holding ten of a limited edition who sends one to a friend loses
all ten, with nothing to tell them why. If the asset was issued **locked**, the
units cannot be reissued — the edition is permanently smaller and nobody chose
that.

The rules most likely to trigger it are the ones an artist would use precisely
because they care about the asset.

---

## 3. What this branch changes

`AssetWallet::assertTransferableAsset(const DigiAsset&)` — new, ~25 lines —
throws `DigiByteException(RPC_MISC_ERROR, …)` when the asset carries a rule the
transfer builder demonstrably cannot satisfy. It is called from
`selectAssetInputs`, before any input is chosen, so `sendasset` and
`sendmanyassets` are both covered by the single path they share.

The rules refused, and why each is certain rather than likely:

| Rule | Why a wallet-built transfer can never satisfy it |
|---|---|
| royalty | the builder adds no royalty output; only issuance calls `addRuleOutputs` |
| required burn (deflation) | the builder adds no burn output |
| required signers | the builder cannot guarantee the required signer inputs are present |

### Deliberately NOT refused

**Vote, KYC and expiry rules are allowed through.** A transfer that satisfies
those is legal — a full send to a valid vote address, or to a KYC-approved
recipient — and refusing them here would break working transfers to prevent
nothing.

This distinction is the part worth reviewing most closely. An over-broad guard
would be worse than the defect, because it would break transfers that succeed
today.

---

## 4. Why this is not a consensus change

**Nothing about how an existing transaction is interpreted changes.** The
clearing behaviour in `DigiByteTransaction.cpp` is untouched. Every transaction
already on chain decodes to exactly the same asset state before and after this
patch. Two nodes, one patched and one not, agree on the ownership of every
asset in every block.

The only difference is that this wallet declines to *create* one particular
kind of transaction.

That is deliberate, and it is why the patch stops here.

### The other fix, and why it is not in this branch

The alternative is to make the indexer refuse the transaction instead of
clearing the outputs — treating a rule-failing transfer as a no-op rather than
a burn. That is the better end state: nobody would lose anything even if a
non-conforming wallet built the transaction.

**But it is a consensus change, not a bug fix.** A node that stopped clearing
those outputs would believe Ray still holds 5 units of
`La9T71BHHeX8fSAnmqnSUkREY3ekJPGiGnhjTW` while every unpatched node believes he
holds 0. Two nodes disagreeing about who owns what is a worse failure than the
one being fixed, and it would need an activation height and coordination that
is not ours to arrange.

We have deliberately not touched it, and we are raising it as a protocol
question rather than submitting it as a patch. This branch is only the half
that is safe to take immediately.

---

## 5. Testing

**Read this section before anything else.** It is deliberately blunt about what
has and has not been run, because a proposal that overstates its testing is
worse than one that admits gaps.

**Verified**

- Every API the patch uses exists on this branch unmodified:
  `DigiAssetRules::empty`, `getIfRequiresRoyalty`, `getRequiredBurn`,
  `getRequiredSignerWeight`, `DigiAsset::getRules`, `DigiAsset::getAssetIndex`.
  `RPC/Server.h` is already included here for the `RPC_` constants, and this
  file already throws `DigiByteException(RPC_MISC_ERROR, …)` elsewhere — the
  patch introduces no new include, type or dependency.
- The equivalent guard has run in our Windows fork since 2026-07-27 without
  incident. That fork's `AssetWallet.cpp` has diverged from this branch, which
  is why this is a fresh patch against your code rather than a cherry-pick.

**NOT verified — we have not compiled this branch**

- **No build has been run against `asset_features`.** Our toolchain targets our
  own fork; this branch has not been through it. Treat the patch as
  API-correct and reviewed by eye, not as compiled.
- **No test run.** The existing suite has not been executed against this
  branch, with or without the patch.
- **Only the royalty rule has a demonstrated victim.** Ray's asset proves that
  one on mainnet. Deflation and required-signer are reasoned from the same code
  path — the transfer builder adds no burn output and cannot guarantee signer
  inputs — but neither has been reproduced.
- **The over-reach case is untested.** That an asset carrying a vote, KYC or
  expiry rule still builds a transfer normally is the regression that matters
  most (§3), and it has been reasoned about rather than exercised.
- **Multi-asset UTXOs.** The guard examines the asset being sent. A companion
  asset sharing the same UTXO is not separately examined, and we have not
  thought through whether that matters for `sendmanyassets`.

If you want any of the above closed before considering it, say which and we
will do that work rather than ask you to take it on trust.

---

## 6. If you would rather not take this

Entirely reasonable, and it costs nothing to say so — the report stands on its
own and the chain evidence in §2 is independently checkable.

If the shape is wrong but the problem is real, two smaller options:

1. Log a warning rather than refusing, so the loss is at least visible.
2. Take only the royalty case, which is the one with a demonstrated mainnet
   victim, and leave burn and signer alone.

We run a Windows fork of DigiAsset Core and have been carrying this guard since
2026-07-27. It has not caused a problem for us, but our usage is narrower than
the project's, which is exactly why this is a proposal rather than a patch
dropped in a release.

---

## 7. Credit

**Ray, [Brasa Studios](https://brasastudios.games)** found this, reproduced it
deliberately on his own asset rather than reporting a hunch, and wrote it up
with the asset id, both transaction ids and both block heights — everything
needed to verify it without taking his word for anything.

He offered to write and submit the fix himself. This branch exists because we
already had the guard running in our fork and could put it in front of you
today; the finding, the reproduction and the analysis of the two possible fixes
are his.

He is building *Elements of War* on DigiAssets, which is how he came to be
holding an asset with a royalty rule in the first place.
