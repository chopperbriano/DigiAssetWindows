//
// Tests for the asset-rule backstop that fundSignSend runs before broadcasting:
// DigiByteTransaction::checkRulesPass() (now public) on the built transfer, using
// the chain context set via setChainContext(). Pure unit tests - no wallet/DB/IPFS.
// The reject path (ruled assets) is covered by the DigiAsset/DigiAssetRules tests;
// here we lock down the critical property that a NORMAL transfer is never rejected.
//

#include "gtest/gtest.h"
#include "DigiAsset.h"
#include "DigiByteTransaction.h"

using namespace std;

namespace {
    DigiAsset makeAsset(uint64_t index, uint64_t count) {
        DigiAsset a("", count, 0, true, DigiAsset::AGGREGABLE); // no rules
        a.setAssetIndex(index);
        return a;
    }
    AssetUTXO makeInput(const string& txid, uint16_t vout, const vector<DigiAsset>& assets) {
        AssetUTXO u;
        u.txid = txid;
        u.vout = vout;
        u.address = "dgb1qsender";
        u.digibyte = 600;
        u.assets = assets;
        return u;
    }
}

// A normal (rule-less) transfer must pass checkRulesPass so the fundSignSend
// backstop never refuses a legitimate send.
TEST(RuleBackstop, AllowsUnruledTransfer) {
    DigiAsset asset = makeAsset(5, 100);
    DigiByteTransaction tx;
    tx.addInput(makeInput("aa11", 0, {asset}));
    DigiAsset sendPart = asset;
    sendPart.setCount(30);
    DigiAsset changePart = asset;
    changePart.setCount(70);
    tx.addDigiAssetOutput("dgb1qrecipient", {sendPart});
    tx.addDigiAssetOutput("dgb1qchange", {changePart});

    tx.setChainContext(20000000, 1700000000);
    EXPECT_FALSE(tx.isIssuance());
    EXPECT_NO_THROW(tx.checkRulesPass());
}

// A full send (no change) of a rule-less asset also passes.
TEST(RuleBackstop, AllowsUnruledFullTransfer) {
    DigiAsset asset = makeAsset(7, 42);
    DigiByteTransaction tx;
    tx.addInput(makeInput("bb22", 0, {asset}));
    tx.addDigiAssetOutput("dgb1qrecipient", {asset}); // send all 42, no change
    tx.setChainContext(20000000, 1700000000);
    EXPECT_NO_THROW(tx.checkRulesPass());
}
