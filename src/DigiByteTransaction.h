//
// Created by mctrivia on 17/06/23.
//
// DigiByteTransaction: the node's decoded view of a single DigiByte
// transaction. Constructed from a txid, it loads the raw transaction, resolves
// each input UTXO's asset content from the Database, copies the outputs, and
// classifies the transaction by inspecting its OP_RETURN: a plain/standard tx,
// a DigiAsset issuance / transfer / burn, a KYC issuance / revoke, an exchange
// rate publication, or an encrypted-key record. For asset transfers it replays
// the on-chain transfer instructions to compute which assets land on which
// outputs (including change and rule enforcement). addToDatabase() persists the
// resulting state (UTXOs, KYC, exchange rates, votes, PSP pinning, domain
// processing); toJSON() renders it for the node's API. This is the core unit
// the chain analyzer processes block by block.
//

#ifndef DIGIASSET_CORE_DIGIBYTETRANSACTION_H
#define DIGIASSET_CORE_DIGIBYTETRANSACTION_H


#include "Database.h"
#include "DigiAsset.h"
#include "DigiAssetTypes.h"
#include <jsonrpccpp/server.h>

// Minimal reference to a transaction output (spent-input identifier).
struct UTXO {
    std::string txid;
    unsigned int vout;
};

/**
 * One decoded DigiByte transaction. Holds its inputs/outputs (with any assets),
 * a type tag (_txType) selecting which of the type-specific members are
 * populated, and the block context (height/hash/time). Instances built from an
 * existing txid are read-only snapshots; the default constructor makes a new,
 * writable transaction.
 */
class DigiByteTransaction {
    const static unsigned int STANDARD = 0;
    const static unsigned int DIGIASSET_ISSUANCE = 1;
    const static unsigned int DIGIASSET_TRANSFER = 2;
    const static unsigned int DIGIASSET_BURN = 3;
    const static unsigned int KYC_ISSUANCE = 10;
    const static unsigned int KYC_REVOKE = 11;
    const static unsigned int EXCHANGE_PUBLISH = 20;
    const static unsigned int ENCRYPTED_KEY = 30;


    std::vector<AssetUTXO> _inputs;
    std::vector<AssetUTXO> _outputs;
    DigiAsset _newAsset;
    unsigned char _txType = STANDARD; //must default to STANDARD since not set in code if STANDARD
    bool _assetFound = false;
    unsigned int _ruleOutputsStart = 0; //vout index of the first rule output(see addRuleOutputs)
    bool _ruleOutputsAdded = false;
    std::vector<DigiAsset> _burns; //amounts destroyed by this tx(burn instructions target output 31)
    bool _unintentionalBurn = false;
    unsigned int _height;
    std::string _txid; //if set tx is not writable(existing)
    std::string _blockHash;
    uint64_t _time; //internal use only it will only be correct for transactions built from tx data or new constructed transactions

    //type KYC_* Only
    KYC _kycData;

    //type ENCRYPTED_KEY and STANDARD only
    std::string _opReturnHex;

    //type Exchange_PUBLISH Only
    std::vector<double> _exchangeRate;

    //type DigiAsset_* Only
    unsigned char _assetTransactionVersion;


    //tx process TestHelpers
    // Each tries to interpret the tx (at the given OP_RETURN vout index) as its
    // type, setting _txType and the relevant members and returning true on a
    // match; false means "not this type, try the next".
    bool decodeAssetTX(const getrawtransaction_t& txData, int dataIndex);
    bool decodeExchangeRate(const getrawtransaction_t& txData, int dataIndex);
    bool decodeKYC(const getrawtransaction_t& txData, int dataIndex);
    bool decodeEncryptedKeyTx(const getrawtransaction_t& txData, int dataIndex);
    // Fallback: records the raw OP_RETURN as an unknown/standard tx.
    void storeUnknown(const getrawtransaction_t& txData, int dataIndex); //todo need to store locally and then add to database when called

    //asset process TestHelpers
    // Replays the encoded transfer instructions to distribute input assets onto
    // outputs (type = issuance/transfer/burn); handles change and rule checks.
    void decodeAssetTransfer(BitIO& dataStream, const std::vector<AssetUTXO>& inputAssets, uint8_t type);
    // Verifies every input asset's rules against this tx; throws on failure.
    void checkRulesPass() const;
    // Places an asset on a given output, merging counts if aggregable.
    void addAssetToOutput(size_t output, const DigiAsset& asset);

    //new transaction building helpers
    void checkWritable() const;
    void buildTransferInstructions(BitIO& data) const;

    friend class DigiByteTransactionBuilder_transferRoundTrip_Test;
    friend class DigiByteTransactionBuilder_transferMultiAssetRoundTrip_Test;
    friend class DigiByteTransactionBuilder_issuanceEncoding_Test;
    friend class DigiByteTransactionBuilder_burnRoundTrip_Test;
    friend class DigiByteTransactionBuilder_burnAllRoundTrip_Test;
    friend class DigiByteTransactionBuilder_burnUnbalancedThrows_Test;

public:
    // Creates a new, writable transaction stamped with the current time.
    explicit DigiByteTransaction();
    // Loads and fully decodes an existing transaction by txid. height is
    // optional (looked up if 0); dontBotherIfNotSpecial lets the chain analyzer
    // skip full input/output processing for txs that clearly carry no assets.
    DigiByteTransaction(const std::string& txid, unsigned int height = 0, bool dontBotherIfNotSpecial = false);

    // Persists this transaction's effects to the Database in one DB transaction.
    void addToDatabase();
    // For an issuance, resolves and back-fills the asset's assetIndex from the DB.
    void lookupAssetIndexes();

    bool isStandardTransaction() const;

    // True if this tx moved no assets and burned none unintentionally.
    bool isNonAssetTransaction() const;
    bool isIssuance() const;
    // True for a transfer; optionally also counts an intentional burn.
    bool isTransfer(bool includeIntentionalBurn = false) const;
    // True for an intentional burn; optionally also counts unintentional burns.
    bool isBurn(bool includeUnintentionalBurn = false) const;
    // True if assets were destroyed as a side effect (e.g. sent to OP_RETURN or
    // a failed transfer), rather than by an explicit burn.
    bool isUnintentionalBurn() const;
    // Returns the asset created by an issuance tx; throws if not an issuance.
    DigiAsset getIssuedAsset() const;

    bool isKYCTransaction() const;
    bool isKYCRevoke() const;
    bool isKYCIssuance() const;
    KYC getKYC() const;

    bool isExchangeTransaction() const;
    size_t getExchangeRateCount() const;
    // Returns the i-th published exchange rate value; throws if out of range.
    double getExchangeRate(uint8_t i) const;
    // Returns address/index/name metadata for the i-th exchange rate, resolving
    // the human-readable name from the standard exchange-rate tables.
    ExchangeRate getExchangeRateName(uint8_t i) const;


    AssetUTXO getInput(size_t n) const;
    AssetUTXO getOutput(size_t n) const;
    unsigned int getInputCount() const;
    unsigned int getOutputCount() const;
    unsigned int getHeight() const;

    /*
     * Functions for building a NEW transaction(not one already on chain).
     * Typical usage(see RPC/Methods/sendasset.cpp and issueasset.cpp):
     *   1) addInput() all asset bearing UTXOs being spent
     *   2) addDigiAssetOutput()/addDigiByteOutput() for all desired outputs(asset outputs must be in first 32)
     *   3) setIssuance() if creating a new asset
     *   4) encodeAssetOpReturn() to get the OP_RETURN payload
     *   5) fund/sign/send via the wallet(see AssetWallet helper)
     */
    bool isWritable() const;
    void addInput(const AssetUTXO& utxo);
    void setIssuance(const DigiAsset& asset);
    void addDigiByteOutput(const std::string& address, uint64_t amount);
    // Adds an output carrying the given DigiAssets to a new/writable tx.
    void addDigiAssetOutput(const std::string& address, const std::vector<DigiAsset>& assets);
    void addAssetBurn(const std::vector<DigiAsset>& assets); //burns these amounts(marks the tx a burn)
    void addRuleOutputs(); //adds the outputs the issued asset's rules require(call after all
                           //asset outputs and before any other extra outputs, e.g. PSP fees)
    std::string encodeAssetOpReturn() const;

    // Test helpers — not for production use
    void setHeightForTesting(unsigned int h) { _height = h; }
    void setIssuanceForTesting() { _txType = DIGIASSET_ISSUANCE; }
    void addOutputForTesting(const std::string& address, uint64_t amount) {
        AssetUTXO utxo;
        utxo.address = address;
        utxo.digibyte = amount;
        _outputs.push_back(utxo);
    }

    // Serializes the whole transaction to JSON for the API, optionally merging
    // into an existing Json value (see the .cpp for the full field list).
    Value toJSON(const Value& original = Json::objectValue) const;



    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */

    // Base class for transaction errors; prefixes "DigiByte Transaction Exception: ".
    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "DigiByte Transaction Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    // Thrown when building a tx that spends more than the available inputs.
    class exceptionNotEnoughFunds : public exception {
    public:
        explicit exceptionNotEnoughFunds()
            : exception("Not enough funds") {}
    };
};


#endif //DIGIASSET_CORE_DIGIBYTETRANSACTION_H
