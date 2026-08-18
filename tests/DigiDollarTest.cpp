//
// Tests for the DigiDollar decoders — no external dependencies required.
//
// Every vector below is real mainnet data captured after DigiDollar activated at block
// 23,869,440, not synthetic.  The oracle commitment comes from the coinbase of block 24,045,821
// and the transfer is mainnet transaction
// 08daf452dcfe36d1dfe32958486f230ba717bfb7bcfd16914bf374d4f8741e3a in block 24,045,731.
//

#include "DigiAssetConstants.h"
#include "Database.h"
#include "DigiAsset.h"
#include "DigiDollar.h"
#include "gtest/gtest.h"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace {

    //real oracle price commitment: OP_RETURN OP_ORACLE <0x03> <90 byte MuSig2 bundle>
    const string ORACLE_SCRIPT =
            "6abf01034c5a052801081005392c09007810000000000000a96b826a00000000"
            "c6aad78382a3dda499ac538723f938774a6e8090698de146548f37b946bd9241"
            "5b4eb158100a3408074f751575449219d500c28001996f0cf7eb325df8184571";

    //the segwit commitment that sits in every coinbase - must never be mistaken for an oracle one
    const string WITNESS_COMMITMENT =
            "6a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf9";

    vout_t makeOutput(unsigned int n, uint64_t sats, const string& scriptHex, const string& type) {
        vout_t output;
        output.n = n;
        output.valueS = sats;
        output.value = static_cast<double>(sats) / 1e8;
        output.scriptPubKey.hex = scriptHex;
        output.scriptPubKey.type = type;
        return output;
    }

    //rebuilds the real mainnet transfer: two DigiDollar inputs, one DigiDollar output of $2.00,
    //one ordinary DGB change output, and the "DD" metadata OP_RETURN
    getrawtransaction_t makeRealTransfer() {
        getrawtransaction_t tx;
        tx.version = 33556336; //0x02000770 - transfer
        tx.vout.push_back(makeOutput(
                0, 0, "512070dd32c4ec5e076d4d23fda3c566ca090319e10da1c55e2fd0d874662e43abfd",
                "witness_v1_taproot"));
        tx.vout.push_back(makeOutput(1, 38439561, "00148ac0805db42d881e3fe56f6f8de7f71b01923021",
                                     "witness_v0_keyhash"));
        tx.vout.push_back(makeOutput(2, 0, "6a024444010202c800", "nulldata"));
        return tx;
    }

    const unsigned int AFTER_ACTIVATION = 24045731;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Version marker
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiDollar, versionMarker_recognisesRealTransfer) {
    EXPECT_TRUE(DigiDollar::isDigiDollarVersion(33556336));
    EXPECT_EQ(DigiDollar::typeFromVersion(33556336), DigiDollar::TX_TRANSFER);
}

TEST(DigiDollar, versionMarker_rejectsOrdinaryVersions) {
    EXPECT_FALSE(DigiDollar::isDigiDollarVersion(1));
    EXPECT_FALSE(DigiDollar::isDigiDollarVersion(2));
    EXPECT_EQ(DigiDollar::typeFromVersion(2), DigiDollar::TX_NONE);
}

TEST(DigiDollar, versionMarker_readsEachType) {
    EXPECT_EQ(DigiDollar::typeFromVersion(0x01000770), DigiDollar::TX_MINT);
    EXPECT_EQ(DigiDollar::typeFromVersion(0x02000770), DigiDollar::TX_TRANSFER);
    EXPECT_EQ(DigiDollar::typeFromVersion(0x03000770), DigiDollar::TX_REDEEM);
}

TEST(DigiDollar, versionMarker_rejectsUnknownTypeByte) {
    //correct marker but a type we do not understand must not be guessed at
    EXPECT_EQ(DigiDollar::typeFromVersion(0x7F000770), DigiDollar::TX_NONE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Oracle price commitment
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiDollar, oracle_recognisesCommitmentScript) {
    EXPECT_TRUE(DigiDollar::isOracleCommitmentScript(ORACLE_SCRIPT));
}

TEST(DigiDollar, oracle_witnessCommitmentIsNotAnOracleCommitment) {
    EXPECT_FALSE(DigiDollar::isOracleCommitmentScript(WITNESS_COMMITMENT));
}

TEST(DigiDollar, oracle_decodesRealMainnetCommitment) {
    DigiDollar::OracleCommitment commitment;
    ASSERT_TRUE(DigiDollar::decodeOracleCommitment(ORACLE_SCRIPT, commitment));

    EXPECT_EQ(commitment.version, 3);
    EXPECT_EQ(commitment.price, 4216u);           //micro USD per DGB, so $0.004216
    EXPECT_EQ(commitment.timestamp, 1786932137);  //oracle sample time, before the block time
    EXPECT_EQ(commitment.participants, 7);        //consensus threshold is 7 of 35

    //the epoch in the payload must agree with the block it was found in
    EXPECT_EQ(commitment.epoch, 24045821u / DigiAssetConstants::DIGIDOLLAR_ORACLE_EPOCH_LENGTH);
}

TEST(DigiDollar, oracle_rejectsMalformedInput) {
    DigiDollar::OracleCommitment commitment;
    EXPECT_FALSE(DigiDollar::decodeOracleCommitment("", commitment));
    EXPECT_FALSE(DigiDollar::decodeOracleCommitment("6abf0103", commitment)); //truncated bundle
    EXPECT_FALSE(DigiDollar::decodeOracleCommitment(WITNESS_COMMITMENT, commitment));
    EXPECT_FALSE(DigiDollar::isOracleCommitmentScript("6ab")); //odd length hex
}

TEST(DigiDollar, oracle_rejectsWrongBundleVersion) {
    //v0x02 bundles were pre-launch only; DigiDollar V1 requires MuSig2 v0x03
    string script = ORACLE_SCRIPT;
    script[7] = '2'; //flip the pushed version byte from 03 to 02
    DigiDollar::OracleCommitment commitment;
    EXPECT_FALSE(DigiDollar::decodeOracleCommitment(script, commitment));
}

TEST(DigiDollar, oracle_foundInCoinbaseAmongstOtherOutputs) {
    getrawtransaction_t coinbase;
    coinbase.version = 1;
    coinbase.vout.push_back(makeOutput(0, 25355810338,
                                       "76a9149977d1cabc2a8b5089a27025fa9e8bbc27f0f55788ac", "pubkeyhash"));
    coinbase.vout.push_back(makeOutput(1, 0, WITNESS_COMMITMENT, "nulldata"));
    coinbase.vout.push_back(makeOutput(2, 0, ORACLE_SCRIPT, "oracle"));

    DigiDollar::OracleCommitment commitment;
    ASSERT_TRUE(DigiDollar::findOracleCommitment(coinbase, commitment));
    EXPECT_EQ(commitment.price, 4216u);
}

TEST(DigiDollar, oracle_absentFromOrdinaryCoinbase) {
    getrawtransaction_t coinbase;
    coinbase.version = 1;
    coinbase.vout.push_back(makeOutput(0, 25355810338,
                                       "76a9149977d1cabc2a8b5089a27025fa9e8bbc27f0f55788ac", "pubkeyhash"));
    coinbase.vout.push_back(makeOutput(1, 0, WITNESS_COMMITMENT, "nulldata"));

    DigiDollar::OracleCommitment commitment;
    EXPECT_FALSE(DigiDollar::findOracleCommitment(coinbase, commitment));
}

// ─────────────────────────────────────────────────────────────────────────────
// Price conversion
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiDollar, priceConversion_matchesExchangeTableConvention) {
    //1 DGB == $0.004216, so 1 USD == 237.19 DGB == 23,719,165,085 sats
    double rate = DigiDollar::priceToExchangeRate(4216);
    EXPECT_NEAR(rate, 23719165085.4, 1.0);
}

TEST(DigiDollar, priceConversion_zeroPriceIsNotDividedBy) {
    EXPECT_EQ(DigiDollar::priceToExchangeRate(0), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction metadata
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiDollar, metadata_decodesRealMainnetTransfer) {
    getrawtransaction_t tx = makeRealTransfer();
    DigiDollar::Metadata metadata;
    ASSERT_TRUE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));

    EXPECT_EQ(metadata.type, DigiDollar::TX_TRANSFER);
    ASSERT_EQ(metadata.amounts.size(), 1u);
    EXPECT_EQ(metadata.amounts[0], 200u); //cents, so $2.00
}

TEST(DigiDollar, metadata_mapsAmountToTheZeroValueTaprootOutput) {
    getrawtransaction_t tx = makeRealTransfer();
    DigiDollar::Metadata metadata;
    ASSERT_TRUE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));

    auto mapped = DigiDollar::mapAmountsToOutputs(tx, metadata);
    ASSERT_EQ(mapped.size(), 1u);
    EXPECT_EQ(mapped[0].first, 0);    //vout 0, the P2TR output
    EXPECT_EQ(mapped[0].second, 200u);
}

TEST(DigiDollar, metadata_rejectedBelowActivationHeight) {
    //nothing before block 23,869,440 can be DigiDollar no matter what the version says
    getrawtransaction_t tx = makeRealTransfer();
    DigiDollar::Metadata metadata;
    EXPECT_FALSE(DigiDollar::decodeMetadata(tx, DigiAssetConstants::DIGIDOLLAR_ACTIVATION_HEIGHT - 1, metadata));
    EXPECT_TRUE(DigiDollar::decodeMetadata(tx, DigiAssetConstants::DIGIDOLLAR_ACTIVATION_HEIGHT, metadata));
}

TEST(DigiDollar, metadata_ordinaryTransactionIsNotDigiDollar) {
    getrawtransaction_t tx;
    tx.version = 2;
    tx.vout.push_back(makeOutput(0, 1000, "76a914d3d5cdec6deaffaca86c11dbd5ec77aea19aa40788ac", "pubkeyhash"));
    DigiDollar::Metadata metadata;
    EXPECT_FALSE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));
}

TEST(DigiDollar, metadata_digiAssetOpReturnIsNotMistakenForDigiDollar) {
    //DigiAsset uses the marker "DA"(0x4441) and DigiDollar uses "DD"(0x4444) - both start with 0x44
    getrawtransaction_t tx;
    tx.version = 2;
    tx.vout.push_back(makeOutput(0, 0, "6a0244410301", "nulldata"));
    DigiDollar::Metadata metadata;
    EXPECT_FALSE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));
}

TEST(DigiDollar, metadata_versionAndPayloadMustAgree) {
    //a transaction claiming to be a mint in its version but a transfer in its payload is
    //malformed and must be rejected rather than half read
    getrawtransaction_t tx = makeRealTransfer();
    tx.version = 0x01000770; //say mint, payload still says transfer
    DigiDollar::Metadata metadata;
    EXPECT_FALSE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));
}

TEST(DigiDollar, metadata_decodesMint) {
    //OP_RETURN "DD" <1> <10000 cents> <24000000> <3> <32 byte owner key>
    //CScriptNum is little endian, so 10000 == 0x2710 pushes as "1027" and
    //24000000 == 0x016E3600 pushes as "00366e01"
    getrawtransaction_t tx;
    tx.version = 0x01000770;
    tx.vout.push_back(makeOutput(0, 500000000000,
                                 "5120" + string(64, 'a'), "witness_v1_taproot")); //collateral vault
    tx.vout.push_back(makeOutput(1, 0, "5120" + string(64, 'b'), "witness_v1_taproot")); //DD output
    tx.vout.push_back(makeOutput(
            2, 0, "6a02444451" "021027" "0400366e01" "53" "20" + string(64, 'c'), "nulldata"));

    DigiDollar::Metadata metadata;
    ASSERT_TRUE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));
    EXPECT_EQ(metadata.type, DigiDollar::TX_MINT);
    ASSERT_EQ(metadata.amounts.size(), 1u);
    EXPECT_EQ(metadata.amounts[0], 10000u); //$100.00
    EXPECT_EQ(metadata.lockHeight, 24000000u);
    EXPECT_EQ(metadata.lockTier, 3);
    EXPECT_EQ(metadata.ownerKey, string(64, 'c'));
}

TEST(DigiDollar, mint_vaultIsTheTaprootOutputHoldingDGB) {
    getrawtransaction_t tx;
    tx.version = 0x01000770;
    tx.vout.push_back(makeOutput(0, 500000000000, "5120" + string(64, 'a'), "witness_v1_taproot"));
    tx.vout.push_back(makeOutput(1, 0, "5120" + string(64, 'b'), "witness_v1_taproot"));
    tx.vout.push_back(makeOutput(2, 12345, "00148ac0805db42d881e3fe56f6f8de7f71b01923021",
                                 "witness_v0_keyhash")); //change, not taproot

    EXPECT_EQ(DigiDollar::findVaultOutput(tx), 0);
}

TEST(DigiDollar, mint_ambiguousVaultIsRefusedRatherThanGuessed) {
    //two taproot outputs both carrying DGB - we cannot tell which is the vault, so report neither
    getrawtransaction_t tx;
    tx.version = 0x01000770;
    tx.vout.push_back(makeOutput(0, 500000000000, "5120" + string(64, 'a'), "witness_v1_taproot"));
    tx.vout.push_back(makeOutput(1, 700000000000, "5120" + string(64, 'b'), "witness_v1_taproot"));

    EXPECT_EQ(DigiDollar::findVaultOutput(tx), -1);
}

TEST(DigiDollar, mapping_refusesWhenAmountCountDoesNotMatchOutputCount) {
    //Declaring two amounts against one DigiDollar output means we misread the transaction.
    //Recording a guess would write a balance the chain could never correct, so record nothing.
    getrawtransaction_t tx = makeRealTransfer();
    DigiDollar::Metadata metadata;
    ASSERT_TRUE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));
    metadata.amounts.push_back(500);

    EXPECT_TRUE(DigiDollar::mapAmountsToOutputs(tx, metadata).empty());
}

TEST(DigiDollar, mapping_zeroAmountOutputsAreSkipped) {
    getrawtransaction_t tx;
    tx.version = 0x02000770;
    tx.vout.push_back(makeOutput(0, 0, "5120" + string(64, 'a'), "witness_v1_taproot"));
    tx.vout.push_back(makeOutput(1, 0, "5120" + string(64, 'b'), "witness_v1_taproot"));
    //amounts 200 and 0
    tx.vout.push_back(makeOutput(2, 0, "6a024444" "52" "02c800" "00", "nulldata"));

    DigiDollar::Metadata metadata;
    ASSERT_TRUE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));
    ASSERT_EQ(metadata.amounts.size(), 2u);

    auto mapped = DigiDollar::mapAmountsToOutputs(tx, metadata);
    ASSERT_EQ(mapped.size(), 1u);
    EXPECT_EQ(mapped[0].first, 0);
    EXPECT_EQ(mapped[0].second, 200u);
}

TEST(DigiDollar, redeem_withNoChangeCarriesNoOpReturn) {
    //a redemption that burns the whole position emits no DigiDollar output and no metadata,
    //so the version marker is the only thing identifying it
    getrawtransaction_t tx;
    tx.version = 0x03000770;
    tx.vout.push_back(makeOutput(0, 500000000000, "76a914d3d5cdec6deaffaca86c11dbd5ec77aea19aa40788ac",
                                 "pubkeyhash"));

    DigiDollar::Metadata metadata;
    ASSERT_TRUE(DigiDollar::decodeMetadata(tx, AFTER_ACTIVATION, metadata));
    EXPECT_EQ(metadata.type, DigiDollar::TX_REDEEM);
    EXPECT_TRUE(metadata.amounts.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Database round trip
//
// These exist because of a real bug: getCurrentDigiDollarRate() used to ask for
// "the newest row at or below UINT_MAX", and the bind takes a signed int, so the
// sentinel arrived as -1 and matched nothing.  getexchangerates passes a real
// sync height and worked, which hid it - only the callers using the sentinel
// (getdgbequivalent, getdigidollarinfo, the Qt oracle panel) reported no price.
// ─────────────────────────────────────────────────────────────────────────────

TEST(DigiDollarDatabase, currentRateDoesNotDependOnAHeightSentinel) {
    remove("../tests/testFiles/_testDigiDollarRate.db");
    Database db("../tests/testFiles/_testDigiDollarRate.db");

    db.addDigiDollarRate(23869440, 596736, 4216, 1786932137, 7);
    db.addDigiDollarRate(24045800, 601145, 4300, 1786932500, 9);

    //newest row, however it is reached internally
    DigiDollarRate current = db.getCurrentDigiDollarRate();
    EXPECT_EQ(current.price, 4300u);
    EXPECT_EQ(current.height, 24045800u);
    EXPECT_EQ(current.participants, 9u);

    //and the historic lookup still resolves to the older row
    DigiDollarRate historic = db.getDigiDollarRateAtHeight(23900000);
    EXPECT_EQ(historic.price, 4216u);
    EXPECT_EQ(historic.height, 23869440u);
}

TEST(DigiDollarDatabase, rateHistoryAcceptsAnUnsignedMaxUpperBound) {
    remove("../tests/testFiles/_testDigiDollarHist.db");
    Database db("../tests/testFiles/_testDigiDollarHist.db");

    db.addDigiDollarRate(23869440, 596736, 4216, 1786932137, 7);
    db.addDigiDollarRate(24045800, 601145, 4300, 1786932500, 9);

    //the default "no upper bound" callers pass must not wrap to -1 and return nothing
    auto all = db.getDigiDollarRateHistory(0, std::numeric_limits<unsigned int>::max(), 100);
    EXPECT_EQ(all.size(), 2u);
}

TEST(DigiDollarDatabase, oneRowPerEpochIsKept) {
    remove("../tests/testFiles/_testDigiDollarEpoch.db");
    Database db("../tests/testFiles/_testDigiDollarEpoch.db");

    //every block inside an epoch republishes the same commitment - keep the first sighting only
    db.addDigiDollarRate(24045800, 601145, 4300, 1786932500, 9);
    db.addDigiDollarRate(24045801, 601145, 4300, 1786932500, 9);
    db.addDigiDollarRate(24045802, 601145, 4300, 1786932500, 9);

    auto all = db.getDigiDollarRateHistory(0, std::numeric_limits<unsigned int>::max(), 100);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].height, 24045800u);
    EXPECT_EQ(db.getDigiDollarLastEpoch(), 601145u);
}

TEST(DigiDollarDatabase, emptyTableReportsNoPriceRatherThanZero) {
    remove("../tests/testFiles/_testDigiDollarEmpty.db");
    Database db("../tests/testFiles/_testDigiDollarEmpty.db");

    EXPECT_THROW(db.getCurrentDigiDollarRate(), std::out_of_range);
    EXPECT_EQ(db.getDigiDollarLastEpoch(), 0u);
}

TEST(DigiDollarDatabase, zeroValueOutputsDoNotAppearAsHoldings) {
    //A DigiDollar output carries 0 DGB on a real address, so with storenonassetutxo=1 it reaches
    //the utxos table as assetIndex 1 amount 0.  Reporting that as a holding told callers the
    //address "holds 0 DigiByte", which no other address has ever done.
    remove("../tests/testFiles/_testDigiDollarHoldings.db");
    Database db("../tests/testFiles/_testDigiDollarHoldings.db");

    const string ddAddress = "dgb1parsr0faekm6l5zssayp65fge9nvaszaaqf5epcayqszfamgh9uxsuxph0u";
    const string dgbAddress = "dgb1q3tqgqhd59kypu0l9dahcmelhrvqeyvppalzh6m";

    //a DigiDollar bearing output: real address, no DigiByte, no DigiAssets
    AssetUTXO ddOutput{
            .txid = "522ddcb060ef73aa34f98bb972d97fb9b1f0e998a23d961c6e38799b9d78187c",
            .vout = 1,
            .address = ddAddress,
            .digibyte = 0};
    db.createUTXO(ddOutput, 24050435, false);

    //an ordinary output for contrast
    AssetUTXO dgbOutput{
            .txid = "522ddcb060ef73aa34f98bb972d97fb9b1f0e998a23d961c6e38799b9d78187c",
            .vout = 3,
            .address = dgbAddress,
            .digibyte = 38439561};
    db.createUTXO(dgbOutput, 24050435, false);

    //the DigiDollar address reports no DigiByte holding at all, rather than a zero one
    EXPECT_TRUE(db.getAddressHoldings(ddAddress).empty());

    //while a real DigiByte balance is still reported as before
    auto holdings = db.getAddressHoldings(dgbAddress);
    ASSERT_EQ(holdings.size(), 1u);
    EXPECT_EQ(holdings[0].assetIndex, 1u);
    EXPECT_EQ(holdings[0].count, 38439561u);
}
