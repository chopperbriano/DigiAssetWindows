//
// Created by mctrivia on 31/07/23.
//
// DigiByteDomain: implements the on-chain ".dgb" domain-name system.
// A single "master domain" DigiAsset carries a metadata document (pinned on
// IPFS) whose "DNS" object maps domain names to the assetId that currently
// owns each name, and whose "next" field can hand off to a successor master
// asset. As the node's chain analyzer processes asset issuances, this class
// watches for issuances of the master domain asset, fetches the updated
// metadata via the IPFS controller, and reconciles the node's local domain
// table (add / revoke domains, or flag the whole system compromised if a
// record is tampered with). It also exposes read-only lookups (domain ->
// assetId / address) used by the node's API. This is a node-side feature; it
// is not used by the pool server.
//

#ifndef DIGIASSET_CORE_DIGIBYTEDOMAIN_H
#define DIGIASSET_CORE_DIGIBYTEDOMAIN_H

#define DIGIBYTEDOMAIN_CALLBACK_NEWMETADATA_ID "DigiByteDomain::_callbackNewMetadata"

#include "DigiAsset.h"
#include "DigiByteTransaction.h"

/**
 * Static utility class for the ".dgb" on-chain domain system. All state lives
 * in the node's Database; this class holds no instance data.
 */
class DigiByteDomain {
    static void initializeClassValues();

public:
    // Called for every asset issuance seen by the chain analyzer. If the asset
    // is the current master domain asset, queues an IPFS download of its
    // metadata so _callbackNewMetadata can reconcile the domain table.
    static void processAssetIssuance(const DigiAsset& asset);

    //API interface calls
    // Returns the assetId currently mapped to the given domain (throws
    // exceptionUnknownDomain / exceptionRevokedDomain via the Database).
    static std::string getAssetId(const std::string& domain);
    // Returns the DigiByte address that currently holds the given domain.
    static std::string getAddress(const std::string& domain);
    // True if the string ends in ".dgb" (syntactic check only, no lookup).
    static bool isDomain(const std::string& domain);

    ///public because needs to be but should only be used by DigiByteDomain.cpp
    // IPFS download callback (registered under DIGIBYTEDOMAIN_CALLBACK_NEWMETADATA_ID).
    // Parses the freshly downloaded master-domain metadata and applies the diff
    // to the local domain table: adds new domains, revokes cleared ones, marks
    // the system compromised on tampering, and follows the "next" master asset.
    static void
    _callbackNewMetadata(const std::string& cid, const std::string& extra, const std::string& content, bool failed);

    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */
    // Base class for all domain-related exceptions; prefixes messages with
    // "DigiByte Domain Exception: ".
    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "DigiByte Domain Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    // Thrown when a looked-up domain exists but has since been revoked.
    class exceptionRevokedDomain : public exception {
    public:
        explicit exceptionRevokedDomain()
            : exception("Domain has been revoked") {}
    };

    // Thrown when a domain's backing asset has been burned.
    class exceptionBurnedDomain : public exception {
    public:
        explicit exceptionBurnedDomain()
            : exception("Domain has been burned") {}
    };

    // Thrown when a domain has never been issued / is not in the table.
    class exceptionUnknownDomain : public exception {
    public:
        explicit exceptionUnknownDomain()
            : exception("Domain has not been issued") {}
    };
};



#endif //DIGIASSET_CORE_DIGIBYTEDOMAIN_H
