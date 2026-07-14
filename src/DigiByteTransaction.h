//
// Created by mctrivia on 17/06/23.
//

#ifndef DIGIASSET_CORE_DIGIBYTETRANSACTION_H
#define DIGIASSET_CORE_DIGIBYTETRANSACTION_H


#include "Database.h"
#include "DigiAsset.h"
#include "DigiAssetTypes.h"
#include <jsonrpccpp/server.h>

struct UTXO {
    std::string txid;
    unsigned int vout;
};

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
    bool _assetFound;
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
    bool decodeAssetTX(const getrawtransaction_t& txData, int dataIndex);
    bool decodeExchangeRate(const getrawtransaction_t& txData, int dataIndex);
    bool decodeKYC(const getrawtransaction_t& txData, int dataIndex);
    bool decodeEncryptedKeyTx(const getrawtransaction_t& txData, int dataIndex);
    void storeUnknown(const getrawtransaction_t& txData, int dataIndex); //todo need to store locally and then add to database when called

    //asset process TestHelpers
    void decodeAssetTransfer(BitIO& dataStream, const std::vector<AssetUTXO>& inputAssets, uint8_t type);
    void checkRulesPass() const;
    void addAssetToOutput(size_t output, const DigiAsset& asset);

    //new transaction building helpers
    void checkWritable() const;
    void buildTransferInstructions(BitIO& data) const;

    friend class DigiByteTransactionBuilder_transferRoundTrip_Test;
    friend class DigiByteTransactionBuilder_transferMultiAssetRoundTrip_Test;
    friend class DigiByteTransactionBuilder_issuanceEncoding_Test;

public:
    explicit DigiByteTransaction();
    DigiByteTransaction(const std::string& txid, unsigned int height = 0, bool dontBotherIfNotSpecial = false);

    void addToDatabase();
    void lookupAssetIndexes();

    bool isStandardTransaction() const;

    bool isNonAssetTransaction() const;
    bool isIssuance() const;
    bool isTransfer(bool includeIntentionalBurn = false) const;
    bool isBurn(bool includeUnintentionalBurn = false) const;
    bool isUnintentionalBurn() const;
    DigiAsset getIssuedAsset() const;

    bool isKYCTransaction() const;
    bool isKYCRevoke() const;
    bool isKYCIssuance() const;
    KYC getKYC() const;

    bool isExchangeTransaction() const;
    size_t getExchangeRateCount() const;
    double getExchangeRate(uint8_t i) const;
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
    void addDigiAssetOutput(const std::string& address, const std::vector<DigiAsset>& assets);
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

    Value toJSON(const Value& original = Json::objectValue) const;



    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */

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

    class exceptionNotEnoughFunds : public exception {
    public:
        explicit exceptionNotEnoughFunds()
            : exception("Not enough funds") {}
    };
};


#endif //DIGIASSET_CORE_DIGIBYTETRANSACTION_H
