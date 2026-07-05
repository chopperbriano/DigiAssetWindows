//
// Created by mctrivia on 31/07/23.
// Updated by RenzoDD on 04/10/23
//
// Implementation of the ".dgb" on-chain domain system (see DigiByteDomain.h).
// On startup a static_block registers _callbackNewMetadata with the IPFS
// controller. As the node's chain analyzer processes issuances,
// processAssetIssuance detects issuances of the master domain asset and asks
// the IPFS controller to fetch its metadata; when the download finishes
// _callbackNewMetadata reconciles the node's local domain table. The remaining
// functions are simple read-only lookups backed by the Database.
//
#include "DigiByteDomain.h"
#include "AppMain.h"
#include "IPFS.h"
#include "Log.h"
#include "static_block.hpp"
#include <iostream>

using namespace std;


// Static block (runs at program init) to register our metadata-download
// callback with the IPFS controller under DIGIBYTEDOMAIN_CALLBACK_NEWMETADATA_ID.
static_block {
    IPFS::registerCallback(DIGIBYTEDOMAIN_CALLBACK_NEWMETADATA_ID, DigiByteDomain::_callbackNewMetadata);
}

/**
 * this function is executed every time an asset is issued so DigiByte Domain can process changes
 * @param asset
 */
void DigiByteDomain::processAssetIssuance(const DigiAsset& asset) {
    //check if an asset we care about and stop processing if not
    AppMain* main = AppMain::GetInstance();
    Database* db = main->getDatabase();
    if (db->isDomainCompromised()) return;
    if (!db->isMasterDomainAssetId(asset.getAssetId())) return;

    //a request to download the metadata will have already been made
    //however metadata may not already be downloaded so lets tell the IPFS controller to run onNewMeta function when its
    //downloaded.  This function will get called in order of issuance even though its asynchronous
    IPFS* ipfs = main->getIPFS();
    ipfs->callOnDownload(asset.getCID(), "DIGIBYTEDOMAIN", "", DIGIBYTEDOMAIN_CALLBACK_NEWMETADATA_ID);
}

/**
 * IPFS download callback fired once the master domain asset's metadata has been
 * fetched. Parses the JSON metadata and reconciles it against the local domain
 * table:
 *   - domains present with an unchanged assetId are skipped
 *   - domains with a cleared (empty) assetId are queued for revocation
 *   - a changed (non-empty, different) assetId is treated as tampering: the
 *     whole domain system is flagged compromised and processing aborts
 *   - domains not previously known are queued as new additions
 * Additions/revocations are only applied after the full integrity scan passes.
 * If the metadata has a "next" field, it becomes the new master domain asset.
 * @param cid - CID of the downloaded metadata (unused here)
 * @param extra - passthrough value (unused here)
 * @param content - raw JSON metadata document
 * @param failed - always false (no maxSleep is ever set for this job)
 * Side effects: mutates the domain table in the Database and writes log entries.
 */
void DigiByteDomain::_callbackNewMetadata(const std::string& cid, const std::string& extra, const std::string& content,
                                          bool failed) {
    ///failed will always be false since no maxSleep ever set
    Log* log = Log::GetInstance();

    //double check domains has not become compromised since this async job was added to the que
    Database* db = AppMain::GetInstance()->getDatabase();
    if (db->isDomainCompromised()) return;

    //decode the contents of the domain in to an easier to read format
    Json::CharReaderBuilder rbuilder;
    Json::Value metadata;
    istringstream s(content);
    string errs;
    Json::parseFromStream(rbuilder, s, &metadata, &errs);

    //process through the list of domains looking for what has changed since the last time
    vector<string> newDomains;
    vector<string> revokedDomains;
    Json::Value V = metadata["DNS"];
    for (const auto& member: V.getMemberNames()) {
        string domain = member;
        string assetId = V[domain].asString();

        try {
            //check if the domain is known and hasn't changed
            string onDB = db->getDomainAssetId(domain, false);
            if (onDB == assetId) continue;

            //check if domain has been revoked or someone has tried to tamper with the DigiByte domain system
            if (assetId.empty()) {
                //the assetId has been deleted
                revokedDomains.push_back(domain);
            } else {
                //someone has tampered with the record
                db->setDomainCompromised();
                log->addMessage("DNS Compromised!!!", Log::INFO);
                return;
            }

        } catch (const DigiByteDomain::exceptionUnknownDomain& e) {
            //brand-new domain so add it
            newDomains.push_back(domain);
        }
    }

    ///the bellow are done after the loop so we can check the integrity of data before making any changes to our database

    //check if new domains has been issued
    if (!newDomains.empty()) {
        for (const auto& domain: newDomains) {
            string assetId = metadata["DNS"][domain].asString();
            db->addDomain(domain, assetId);
            log->addMessage("Domain added: " + domain, Log::INFO);
        }
    }

    //check if a domain has been revoked
    if (!revokedDomains.empty()) {
        for (const auto& domain: revokedDomains) {
            db->revokeDomain(domain);
            log->addMessage("Domain revoked: " + domain, Log::INFO);
        }
    }

    //check if a new dns is published
    if (metadata.isMember("next") && metadata["next"].isString()) {
        db->setMasterDomainAssetId(metadata["next"].asString());
        log->addMessage("New DNS added " + metadata["next"].asString(), Log::INFO);
    }
}

std::string DigiByteDomain::getAddress(const string& domain) {
    Database* db = AppMain::GetInstance()->getDatabase();
    return db->getDomainAddress(domain);
}

std::string DigiByteDomain::getAssetId(const string& domain) {
    Database* db = AppMain::GetInstance()->getDatabase();
    return db->getDomainAssetId(domain);
}

bool DigiByteDomain::isDomain(const string& domain) {
    if (domain.size() < 4) {
        return false;
    }
    return domain.substr(domain.size() - 4) == ".dgb";
}
