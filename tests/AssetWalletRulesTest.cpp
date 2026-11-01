//
// Tests for the wallet side guard that refuses transfers of assets whose rules a wallet built
// transaction can never satisfy.
//
// Background(issue 29): addRuleOutputs() is called from issueasset only - nothing in the transfer
// path adds rule outputs.  A transfer of an asset with, say, a royalty rule therefore fails
// DigiAsset::checkRulesPass when it is decoded, and the handler for that failure clears the asset
// from *every* output in the transaction, the change output included.  Sending 1 of 5 units
// destroyed all 5, silently.  AssetWallet::assertTransferableAsset refuses up front instead.
//

#include "AppMain.h"
#include "AssetWallet.h"
#include "Database.h"
#include "DigiAsset.h"
#include "DigiAssetRules.h"
#include "DigiByteCore_Exception.h"
#include "gtest/gtest.h"

using namespace std;

namespace {

    ///Builds an asset carrying the given rules.  Values match the database constructor's shape;
    ///only the rules matter here.
    DigiAsset assetWithRules(const DigiAssetRules& rules) {
        DigiAsset asset(1, "La9T71BHHeX8fSAnmqnSUkREY3ekJPGiGnhjTW", "", KYC(), rules, 1, 1, 5);
        asset.setCount(5);
        return asset;
    }

    ///Chain state for the guard tests.  Passing it in keeps them free of a database entirely.
    const unsigned int CHAIN_HEIGHT = 24106276;
    const uint64_t NOW_SECONDS = 1756252800; //2025-08-27, after the 2020 expiry used below

} // namespace

///checkRulesPass reads exchange rates out of the database, so those two tests need one.
///It is removed afterwards rather than left in tests/testFiles, which .gitignore does not
///cover at this depth.
class AssetWalletRulesDb : public ::testing::Test {
protected:
    static constexpr const char* DB_PATH = "../tests/testFiles/_assetwallet_rules_test.db";
    Database* db = nullptr;

    void SetUp() override {
        cleanup();
        db = new Database(DB_PATH);
        AppMain::GetInstance()->setDatabase(db);
    }

    void TearDown() override {
        delete db;
        db = nullptr;
        cleanup();
    }

    static void cleanup() {
        remove(DB_PATH);
        remove((string(DB_PATH) + "-wal").c_str());
        remove((string(DB_PATH) + "-shm").c_str());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Rules no wallet built transfer can satisfy - must be refused
// ─────────────────────────────────────────────────────────────────────────────

TEST(AssetWalletRules, royaltyIsRefused) {
    DigiAssetRules rules;
    rules.setRoyalties({{.address = "dgb1qlk3hldeynl3prqw259u8gv0jh7w5nwppxlvt3v", .amount = 10000000}});
    EXPECT_THROW(AssetWallet::assertTransferableAsset(assetWithRules(rules), CHAIN_HEIGHT, NOW_SECONDS), DigiByteException);
}

TEST(AssetWalletRules, requiredBurnIsRefused) {
    DigiAssetRules rules;
    rules.setDeflationary(1);
    EXPECT_THROW(AssetWallet::assertTransferableAsset(assetWithRules(rules), CHAIN_HEIGHT, NOW_SECONDS), DigiByteException);
}

TEST(AssetWalletRules, requiredSignerIsRefused) {
    DigiAssetRules rules;
    rules.setRequireSigners(1, {{.address = "dgb1qlk3hldeynl3prqw259u8gv0jh7w5nwppxlvt3v", .weight = 1}});
    EXPECT_THROW(AssetWallet::assertTransferableAsset(assetWithRules(rules), CHAIN_HEIGHT, NOW_SECONDS), DigiByteException);
}

TEST(AssetWalletRules, expiredIsRefused) {
    DigiAssetRules rules;
    rules.setExpiry(DigiAssetRules::MIN_EPOCH_VALUE); //2020-01-01, long past
    EXPECT_THROW(AssetWallet::assertTransferableAsset(assetWithRules(rules), CHAIN_HEIGHT, NOW_SECONDS), DigiByteException);
}

// ─────────────────────────────────────────────────────────────────────────────
// Rules a transfer CAN satisfy - must keep working
// ─────────────────────────────────────────────────────────────────────────────

TEST(AssetWalletRules, plainAssetIsAllowed) {
    EXPECT_NO_THROW(AssetWallet::assertTransferableAsset(assetWithRules(DigiAssetRules()), CHAIN_HEIGHT, NOW_SECONDS));
}

TEST(AssetWalletRules, kycRestrictionIsAllowed) {
    //a send to an allowed address is legal, so this must not be blanket refused
    DigiAssetRules rules;
    rules.setRequireKYC();
    EXPECT_NO_THROW(AssetWallet::assertTransferableAsset(assetWithRules(rules), CHAIN_HEIGHT, NOW_SECONDS));
}

TEST(AssetWalletRules, futureExpiryIsAllowed) {
    DigiAssetRules rules;
    //ms since epoch, far enough out that the test does not rot
    rules.setExpiry(4102444800000ULL); //2100-01-01
    EXPECT_NO_THROW(AssetWallet::assertTransferableAsset(assetWithRules(rules), CHAIN_HEIGHT, NOW_SECONDS));
}

// ─────────────────────────────────────────────────────────────────────────────
// The underlying trap this guard exists to avoid.  checkRulesPass had no coverage at all,
// which is how the burn shipped.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(AssetWalletRulesDb, transferWithoutRoyaltyOutputFailsTheRules) {

    DigiAssetRules rules;
    rules.setRoyalties({{.address = "dgb1royaltyaddressthatisnevrpaid00000000", .amount = 10000000}});
    DigiAsset asset = assetWithRules(rules);

    //what sendasset builds: the asset moves, nothing pays the royalty address
    AssetUTXO input;
    input.address = "dgb1senderaddress0000000000000000000000";
    input.digibyte = 100000;
    input.assets.push_back(asset);

    AssetUTXO toRecipient;
    toRecipient.address = "dgb1recipientaddress00000000000000000";
    toRecipient.digibyte = 1000;
    DigiAsset sent = asset;
    sent.setCount(1);
    toRecipient.assets.push_back(sent);

    AssetUTXO change;
    change.address = input.address;
    change.digibyte = 1000;
    DigiAsset kept = asset;
    kept.setCount(4);
    change.assets.push_back(kept);

    EXPECT_THROW(asset.checkRulesPass({input}, {toRecipient, change}, 24106276, 1756252800),
                 DigiAsset::exceptionRuleFailed);
}

//Control for the test above: it has to fail because the royalty is unpaid, not because
//checkRulesPass throws for everything.  Same transaction plus an output paying the royalty
//address must pass, otherwise the test above proves nothing.
TEST_F(AssetWalletRulesDb, transferWithRoyaltyOutputPassesTheRules) {

    const string royaltyAddress = "dgb1royaltyaddressthatisnevrpaid00000000";
    DigiAssetRules rules;
    rules.setRoyalties({{.address = royaltyAddress, .amount = 10000000}});
    DigiAsset asset = assetWithRules(rules);

    AssetUTXO input;
    input.address = "dgb1senderaddress0000000000000000000000";
    input.digibyte = 1000000000;
    input.assets.push_back(asset);

    AssetUTXO toRecipient;
    toRecipient.address = "dgb1recipientaddress00000000000000000";
    toRecipient.digibyte = 1000;
    DigiAsset sent = asset;
    sent.setCount(1);
    toRecipient.assets.push_back(sent);

    AssetUTXO change;
    change.address = input.address;
    change.digibyte = 1000;
    DigiAsset kept = asset;
    kept.setCount(4);
    change.assets.push_back(kept);

    //the output issueasset's addRuleOutputs() would have added and the transfer path never does
    AssetUTXO royaltyPayment;
    royaltyPayment.address = royaltyAddress;
    royaltyPayment.digibyte = 1000000000; //comfortably over any rate derived minimum

    EXPECT_NO_THROW(asset.checkRulesPass({input}, {toRecipient, change, royaltyPayment}, 24106276, 1756252800));
}

// ─────────────────────────────────────────────────────────────────────────────
// Approval rule: the approver signs by spending a coin of their own - usually plain DGB, which a
// node that does not store non-asset UTXOs has no record of.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
    const string APPROVER = "dgb1qlk3hldeynl3prqw259u8gv0jh7w5nwppxlvt3v";

    //asset input from the holder, a plain DGB input from the approver, the asset to a recipient
    void approvalTransfer(const DigiAsset& asset, const string& approverInputAddress,
                          vector<AssetUTXO>& inputs, vector<AssetUTXO>& outputs) {
        AssetUTXO holder;
        holder.txid = "aa00000000000000000000000000000000000000000000000000000000000001";
        holder.vout = 0;
        holder.address = "dgb1holderaddress00000000000000000000000";
        holder.digibyte = 100000;
        holder.assets.push_back(asset);

        AssetUTXO approver; //no assets: plain DGB
        approver.txid = "bb00000000000000000000000000000000000000000000000000000000000002";
        approver.vout = 1;
        approver.address = approverInputAddress;
        approver.digibyte = 100000;

        AssetUTXO toRecipient;
        toRecipient.address = "dgb1recipientaddress00000000000000000";
        toRecipient.digibyte = 1000;
        toRecipient.assets.push_back(asset);

        inputs = {holder, approver};
        outputs = {toRecipient};
    }
}

//Control: an approver input whose address is known counts, so the transfer passes.
TEST_F(AssetWalletRulesDb, approverSpendingPlainDgbIsCounted) {
    DigiAssetRules rules;
    rules.setRequireSigners(1, {{.address = APPROVER, .weight = 1}});
    DigiAsset asset = assetWithRules(rules);
    vector<AssetUTXO> inputs, outputs;
    approvalTransfer(asset, APPROVER, inputs, outputs);
    EXPECT_NO_THROW(asset.checkRulesPass(inputs, outputs, 24106276, 1756252800));
}

//A pruned node hands that input back with no address.  It used to be matched as-is, so the
//approval went uncounted and the transfer failed "Signers" - destroying its assets on this fork
//while upstream, which asks DigiByte Core for the address, moved them.  Now the address is asked
//of Core; with no Core to ask it must fail loudly (sync retries), never as a rule failure.
TEST_F(AssetWalletRulesDb, approverInputWithNoStoredAddressIsNotSilentlyUncounted) {
    DigiAssetRules rules;
    rules.setRequireSigners(1, {{.address = APPROVER, .weight = 1}});
    DigiAsset asset = assetWithRules(rules);
    vector<AssetUTXO> inputs, outputs;
    approvalTransfer(asset, "", inputs, outputs);
    EXPECT_THROW(asset.checkRulesPass(inputs, outputs, 24106276, 1756252800), std::runtime_error);
}
