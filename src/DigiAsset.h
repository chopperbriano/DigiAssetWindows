//
// Created by mctrivia on 07/06/23.
//

// DigiAsset.h - Declares the DigiAsset class, the in-memory representation of a single DigiAsset
// (or a quantity of one) as tracked by the node's chain analyzer. It knows how to decode an asset
// out of a DigiByte transaction's OP_RETURN payload (issuance), how to rebuild itself from a
// database row, and how to serialize itself to JSON for the API. It carries the asset's identity
// (assetId, on-chain metadata CID, issuer KYC), its transfer rules (DigiAssetRules), and helpers to
// enforce those rules on a set of transaction inputs/outputs. Both the node and the pool server use
// this class; the pool server additionally queries Permanent Storage Pool membership (getPspMembership)
// and whether a pool has flagged the asset as bad (isBad).

#ifndef DIGIASSET_CORE_DIGIASSET_H
#define DIGIASSET_CORE_DIGIASSET_H

#define DIGIASSET_JSON_IPFS_MAX_WAIT 1000

//todo check if min length is longer (OP_RETURN - 8bit)(OP_RETURN LENGTH - 8 bit)(DigiAsset Header - 16 bit)(version - 8 bit)(OP_CODE - 8bit)?
#define DIGIASSET_MIN_POSSIBLE_LENGTH 48

#include "BitIO.h"
#include "Database.h"
#include "DigiAssetRules.h"
#include "DigiAssetTypes.h"
#include "KYC.h"
#include <jsonrpccpp/server.h>
#include <map>
#include <string>

// Represents one DigiAsset type (and optionally a held quantity of it). Instances are created three
// ways: decoded from an issuance transaction, rebuilt from the database, or default-constructed and
// filled in. Assets loaded from chain/DB are write-protected (_enableWrite=false) unless the owner
// calls setOwned(); this guards against accidentally mutating consensus-derived data.
class DigiAsset {

    bool _existingAsset = false; //set to true if an existing asset
    bool _enableWrite = true;    //set to false if existing asset.  can be turned back to true if unlocked asset

    //asset data
    std::string _assetId;
    std::string _cid;
    DigiAssetRules _rules;
    unsigned char _divisibility;
    bool _locked;
    unsigned char _aggregation;
    unsigned int _heightCreated;
    unsigned int _heightUpdated;

    //database reference
    uint64_t _assetIndex = 0; //0=not yet created

    //issuer data
    KYC _issuer;

    //count(optional to allow including a count of specific asset type)
    uint64_t _count = 0;

    //count(optional to allow including the total count of specific asset type)
    int64_t _initialCount = -1;

    //array of PSP that asset is a member of.  [-1] means we have not looked it up yet
    std::vector<int> _pspMembership = {-1};

    //functions to help process chain data
    // Decodes an issuance transaction body into this object; returns false if it is not a valid issuance.
    bool
    processIssuance(const getrawtransaction_t& txData, unsigned int height, unsigned char version, unsigned char opcode,
                    BitIO& dataStream);
    // Derives the base58 assetId from the issuing transaction's first input and the issuance flags.
    std::string calculateAssetId(const vin_t& firstVin, uint8_t issuanceFlags) const;
    // Reconstructs the simplified scriptPubKey used to hash unlocked-asset ids from a transaction input.
    static std::vector<uint8_t> calcSimpleScriptPubKey(const vin_t& vinData);
    // Writes ripemd160(sha256(data)) (20 bytes) into result starting at startIndex. Two overloads for
    // string and byte-vector inputs.
    static void insertSRHash(const std::string& dataToHash, std::vector<uint8_t>& result, size_t startIndex);
    static void insertSRHash(std::vector<uint8_t> dataToHash, std::vector<uint8_t>& result, size_t startIndex);
    // For unlocked aggregable assets, reconciles rules encoded in this issuance against any pre-existing
    // rules in the database (older rules win unless they were declared rewritable).
    void handleRulesConflict();

    friend class DigiAsset_calcSimpleScriptPubKey_Test;

    friend class DigiAsset_calculateAssetId_Test;

public:
    //constants
    static const unsigned int EXCHANGE_RATE_LENIENCY = 240; //number of blocks off exchange rate can be and still be excepted
    static const unsigned char AGGREGABLE = 0;
    static const unsigned char HYBRID = 1;
    static const unsigned char DISPERSED = 2;

    //constructors
    DigiAsset() = default;
    // Constructs an asset by decoding an issuance transaction; throws exceptionInvalidIssuance if the
    // transaction does not describe a valid issuance.
    DigiAsset(const getrawtransaction_t& txData, unsigned int height, unsigned char version,
              unsigned char opcode, BitIO& dataStream);

    //helper functions for preprocessing asset
    // Inspects a transaction's OP_RETURN output for a DigiAsset header; outputs the version, opcode
    // (0 if not a DigiAsset tx) and a BitIO stream positioned just past the 32-bit header.
    static void decodeAssetTxHeader(const getrawtransaction_t& txData, unsigned char& version, unsigned char& opcode,
                                    BitIO& dataStream);

    //constructor intended for use by Database only
    // Rebuilds an asset from stored database fields (write-protected existing asset).
    DigiAsset(uint64_t assetIndex, const std::string& assetId, const std::string& cid, const KYC& issuer,
              const DigiAssetRules& rules,
              unsigned int heightCreated, unsigned int heightUpdated, uint64_t amount);

    //comparison
    bool operator==(const DigiAsset& rhs) const;
    bool operator!=(const DigiAsset& rhs) const;

    void removeCount(uint64_t count);
    void setCount(uint64_t count);
    void addCount(uint64_t count);
    uint64_t getCount() const;
    std::string getStrCount() const;
    uint8_t getDecimals() const;

    uint64_t getInitialCount();
    std::vector<int> getPspMembership();

    uint64_t getAssetIndex(bool allowUnknownAssetIndex = false) const;
    bool isAssetIndexSet() const;
    void lookupAssetIndex(const std::string& txid, unsigned int vout);
    void setAssetIndex(uint64_t assetIndex);
    std::string getAssetId() const;
    std::string getCID() const;
    KYC getIssuer() const;
    DigiAssetRules getRules() const;
    unsigned int getHeightCreated() const;
    unsigned int getHeightUpdated() const;
    uint64_t getExpiry() const;
    bool isBad(int poolIndex = -1) const;


    bool isHybrid() const;
    bool isAggregable() const;
    bool isDispersed() const;
    bool isLocked() const;

    //functions that can only be used on assets we own and can be edited(or new assets not on chain)
    void setOwned();
    void setRules(const DigiAssetRules& rules);
    void
    checkRulesPass(const std::vector<AssetUTXO>& inputs, const std::vector<AssetUTXO>& outputs, unsigned int height,
                   uint64_t time) const;

    Value toJSON(bool simplified = false, bool ignoreIPFS = false) const;


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
            _fullErrorMessage = "DigiAsset Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    class exceptionInvalidIssuance : public exception {
    public:
        explicit exceptionInvalidIssuance()
            : exception("Invalid Issuance") {}
    };

    class exceptionInvalidTransfer : public exception {
    public:
        explicit exceptionInvalidTransfer(const std::string& error = "Invalid Transfer")
            : exception(error) {}
    };

    class exceptionWriteProtected : public exception {
    public:
        explicit exceptionWriteProtected()
            : exception("IAsset value is write protected") {} //running setOwned may fix problem if it doesn't value can not be changed
    };

    class exceptionUnknownAssetIndex : public exception {
    public:
        explicit exceptionUnknownAssetIndex()
            : exception("The asset has either not been added to database yet or have not looked it up yet") {}
    };

    class exceptionInvalidMetaData : public exception {
    public:
        explicit exceptionInvalidMetaData()
            : exception("MetaData Invalid") {}
    };

    class exceptionRuleFailed : public exceptionInvalidTransfer {
    public:
        explicit exceptionRuleFailed(const std::string& rule)
            : exceptionInvalidTransfer("Transaction failed because " + rule + " rule failed") {}
    };
};

#endif //DIGIASSET_CORE_DIGIASSET_H
