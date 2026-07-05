//
// Created by mctrivia on 06/06/23.
//
// KYC.h - declares the KYC class, the node's model of an on-chain
// "Know Your Customer" verification record for a DigiByte address.
//
// A KYC verify transaction (published by a trusted verifier address) binds a
// wallet address to a country code plus either a plain-text name or a 32-byte
// identity hash, effective from the block height it was mined at. A later
// revoke transaction cancels that binding at a given height. This class decodes
// those transactions from raw tx data, stores the resulting record, and answers
// whether the address was KYC-valid at a particular chain height. Used by the
// chain analyzer to gate/label DigiAsset issuers that require KYC.
//

#ifndef DIGIASSET_CORE_KYC_H
#define DIGIASSET_CORE_KYC_H


#include "DigiByteCore_Types.h"
#include <functional>

class KYC {
    std::string _address;
    std::string _name;
    std::string _country;
    std::string _hash;
    int _heightCreated = -1;
    int _heightRevoked = -1;

    // Try to decode txData as a KYC verify record; on success stores the record and returns true.
    bool processKYCVerify(const getrawtransaction_t& txData, unsigned int height,
                          std::function<std::string(std::string, unsigned int)>& addressGetterFunction);
    // Try to decode txData as a KYC revoke record; on success marks this record revoked and returns true.
    bool processKYCRevoke(const getrawtransaction_t& txData, unsigned int height,
                          std::function<std::string(std::string, unsigned int)>& addressGetterFunction);
    // Returns true if the given address is an authorized KYC verifier at the given height.
    static bool isKYCVerifier(const std::string& address, unsigned int height);

public:
    KYC() = default;
    // Construct an empty record holding only an address (no verification data yet).
    KYC(const std::string& address);
    // Construct a fully-populated record directly from stored fields (e.g. loaded from the database).
    KYC(const std::string& address, const std::string& country, const std::string& name, const std::string& hash,
        unsigned int heightCreated, int heightRevoked = -1);
    // Construct a record by decoding a raw transaction (see processTX).
    KYC(const getrawtransaction_t& txData, unsigned int height,
        std::function<std::string(std::string, unsigned int)> addressGetterFunction);

    unsigned int processTX(const getrawtransaction_t& txData, unsigned int height,
                           std::function<std::string(std::string, unsigned int)> addressGetterFunction);
    std::string getAddress() const;
    std::string getName() const;
    std::string getHash() const;
    std::string getCountry() const;
    unsigned int getHeightCreated() const;
    int getHeightRevoked() const;      //-1 not yet revoked or haven't yet processed
    bool valid(int height = -1) const; //-1 highest scanned(returns false if empty also)
    bool empty() const;

    const static unsigned int NA = 0;
    const static unsigned int VERIFY = 1;
    const static unsigned int REVOKE = 2;


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
            _fullErrorMessage = "KYC Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    class exceptionUnknownValue : public exception {
    public:
        explicit exceptionUnknownValue()
            : exception("value unknown") {}
    };
};


#endif //DIGIASSET_CORE_KYC_H
