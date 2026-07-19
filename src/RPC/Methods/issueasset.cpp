//
// Created by DigiAsset Core on 14/07/26.
//

#include "AppMain.h"
#include "AssetWallet.h"
#include "DigiByteDomain.h"
#include "DigiByteTransaction.h"
#include "IPFS.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <algorithm>
#include <jsoncpp/json/value.h>
#include <jsoncpp/json/writer.h>

namespace RPC {
    namespace Methods {
        /**
        * Creates(issues) a new DigiAsset.  The metadata gets published to IPFS, the issuance
        * transaction is funded, signed and broadcast by the wallet.
        *
        * params[0] - object describing the asset:
        *   "name"(string required) - asset name
        *   "amount"(integer, decimal number or numeric string required) - number of assets to
        *            create in display units("1.5" with 2 decimals creates 150 of the smallest unit)
        *   "decimals"(integer 0-7 optional default 0) - number of decimal places
        *   "locked"(bool optional default true) - true means no more can ever be issued
        *   "aggregation"(string optional default "aggregable") - one of "aggregable", "hybrid", "dispersed"
        *   "description"(string optional) - description stored in the metadata
        *   "urls"(array optional) - array of url objects({"name":..,"url":..,"mimeType":..}) stored in the metadata
        *   "userData"(object optional) - any extra data to store in the metadata
        *   "toAddress"(string optional) - address or domain the new assets should be sent to.
        *                                  Defaults to a new wallet address
        *   "metadata"(object optional) - advanced: full metadata object to publish as is.  When used,
        *                                 name/description/urls/userData are ignored
        *   "psp"(optional default true) - which permanent storage pool(s) store the metadata(and any
        *                                 IPFS urls it references).  The public pool charges $1.20 USD
        *                                 per MB, paid in DGB at the current on-chain exchange rate as
        *                                 an extra output on the issuance.  Accepts:
        *                                    true - the public pool(default)
        *                                    false - no pool.  Issuances that don't pay in to a pool
        *                                            are not recognised by most of the ecosystem, so
        *                                            only use if you know what you are doing
        *                                    integer - a single pool index(see getpsp)
        *                                    array of integers - pay in to several pools
        *
        * Note: rule encoding(royalties, approval lists, vote, expiry, etc) is not supported yet.
        *
        * @return object:
        *   "txid"(string) - txid of the issuance transaction
        *   "assetId"(string) - id of the newly created asset
        *   "cid"(string) - IPFS cid of the published metadata
        */
        extern const Response issueasset(const Json::Value& params) {
            if ((params.size() != 1) || !params[0].isObject()) {
                throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            }
            const Json::Value& config = params[0];

            AppMain* main = AppMain::GetInstance();

            //required fields
            if (!config.isMember("name") || !config["name"].isString() || config["name"].asString().empty()) {
                throw DigiByteException(RPC_INVALID_PARAMS, "name is required");
            }
            if (!config.isMember("amount")) throw DigiByteException(RPC_INVALID_PARAMS, "amount is required");

            //optional fields
            unsigned char decimals = 0;
            if (config.isMember("decimals")) {
                if (!config["decimals"].isInt() || (config["decimals"].asInt() < 0) || (config["decimals"].asInt() > 7)) {
                    throw DigiByteException(RPC_INVALID_PARAMS, "decimals must be an integer from 0 to 7");
                }
                decimals = static_cast<unsigned char>(config["decimals"].asInt());
            }
            bool locked = true;
            if (config.isMember("locked")) {
                if (!config["locked"].isBool()) throw DigiByteException(RPC_INVALID_PARAMS, "locked must be a bool");
                locked = config["locked"].asBool();
            }
            unsigned char aggregation = DigiAsset::AGGREGABLE;
            if (config.isMember("aggregation")) {
                std::string aggregationStr = config["aggregation"].asString();
                if (aggregationStr == "aggregable") {
                    aggregation = DigiAsset::AGGREGABLE;
                } else if (aggregationStr == "hybrid") {
                    aggregation = DigiAsset::HYBRID;
                } else if (aggregationStr == "dispersed") {
                    aggregation = DigiAsset::DISPERSED;
                } else {
                    throw DigiByteException(RPC_INVALID_PARAMS, "aggregation must be aggregable, hybrid or dispersed");
                }
            }

            //amount(in display units)
            uint64_t amount;
            try {
                amount = AssetWallet::parseAssetAmount(config["amount"], decimals);
            } catch (const std::out_of_range& e) {
                throw DigiByteException(RPC_INVALID_PARAMS, e.what());
            }

            //rules are not supported yet - fail loudly instead of silently ignoring them
            if (config.isMember("rules")) {
                throw DigiByteException(RPC_INVALID_PARAMS, "rule encoding is not supported yet");
            }

            //permanent storage pool participation(default: the public pool.  Unstored assets aren't
            //recognised by most of the ecosystem)
            std::vector<unsigned int> pspPools{1}; //pool 1 = public pool
            if (config.isMember("psp")) {
                const Json::Value& pspVal = config["psp"];
                pspPools.clear();
                if (pspVal.isBool()) {
                    if (pspVal.asBool()) pspPools.push_back(1);
                } else if (pspVal.isUInt()) {
                    pspPools.push_back(pspVal.asUInt());
                } else if (pspVal.isArray()) {
                    for (const Json::Value& v: pspVal) {
                        if (!v.isUInt()) throw DigiByteException(RPC_INVALID_PARAMS, "psp array must contain pool indexes");
                        pspPools.push_back(v.asUInt());
                    }
                } else {
                    throw DigiByteException(RPC_INVALID_PARAMS, "psp must be a bool, a pool index or an array of pool indexes");
                }
                std::sort(pspPools.begin(), pspPools.end());
                pspPools.erase(std::unique(pspPools.begin(), pspPools.end()), pspPools.end());
                for (unsigned int poolIndex: pspPools) {
                    if (poolIndex >= main->getPermanentStoragePoolList()->getPoolCount()) {
                        throw DigiByteException(RPC_INVALID_PARAMS, "psp pool index out of range - see getpsp");
                    }
                }
            }

            //where the new assets should go
            std::string toAddress;
            if (config.isMember("toAddress")) {
                toAddress = config["toAddress"].asString();
                if (DigiByteDomain::isDomain(toAddress)) {
                    toAddress = DigiByteDomain::getAddress(toAddress);
                }
            } else {
                toAddress = main->getDigiByteCore()->getnewaddress();
            }

            //build the metadata document
            Json::Value metadata;
            if (config.isMember("metadata")) {
                if (!config["metadata"].isObject()) throw DigiByteException(RPC_INVALID_PARAMS, "metadata must be an object");
                metadata = config["metadata"];
            } else {
                Json::Value data = Json::objectValue;
                data["assetName"] = config["name"];
                if (config.isMember("description")) data["description"] = config["description"];
                if (config.isMember("urls")) {
                    if (!config["urls"].isArray()) throw DigiByteException(RPC_INVALID_PARAMS, "urls must be an array");
                    data["urls"] = config["urls"];
                } else {
                    data["urls"] = Json::arrayValue; //pools require data.urls to exist to price storage
                }
                if (config.isMember("userData")) data["userData"] = config["userData"];
                metadata = Json::objectValue;
                metadata["data"] = data;
            }

            //publish the metadata to IPFS
            Json::FastWriter writer;
            std::string cid;
            try {
                cid = main->getIPFS()->addFile(writer.write(metadata));
            } catch (const IPFS::exception& e) {
                throw DigiByteException(RPC_MISC_ERROR, std::string("Failed to publish metadata to IPFS: ") + e.what());
            }

            //build the issuance transaction
            DigiAsset asset;
            try {
                asset = DigiAsset(cid, amount, decimals, locked, aggregation);
            } catch (const std::out_of_range& e) {
                throw DigiByteException(RPC_INVALID_PARAMS, e.what());
            }
            DigiByteTransaction tx;
            tx.setIssuance(asset);
            DigiAsset newAssets = asset; //full amount to the recipient
            tx.addDigiAssetOutput(toAddress, std::vector<DigiAsset>{newAssets});

            //add the permanent storage pool payments(must be the last change to the tx before funding)
            for (unsigned int poolIndex: pspPools) {
                try {
                    main->getPermanentStoragePoolList()->getPool(poolIndex)->enable(tx);
                } catch (const DigiAsset::exceptionInvalidMetaData& e) {
                    throw DigiByteException(RPC_INVALID_PARAMS, "metadata is not compatible with the storage pool(data.urls must be an array and all urls must be on IPFS)");
                } catch (const PermanentStoragePool::exceptionCantEnablePSP& e) {
                    throw DigiByteException(RPC_MISC_ERROR, "could not enable permanent storage on this issuance");
                }
            }

            //fund, sign and broadcast
            std::string signedHex;
            std::string txid = AssetWallet::fundSignSend(tx, &signedHex);

            //compute the new assetId from the first input of the signed transaction
            std::string assetId;
            try {
                decoderawtransaction_t decoded = main->getDigiByteCore()->decoderawtransaction(signedHex);
                assetId = asset.calculateAssetId(decoded.vin[0], asset.getIssuanceFlags());
            } catch (const std::exception& e) {
                //asset was issued fine - the id can be looked up once the tx confirms
                assetId = "";
            }

            //return response
            Json::Value result = Json::objectValue;
            result["txid"] = txid;
            result["assetId"] = assetId;
            result["cid"] = cid;
            Response response;
            response.setResult(result);
            response.setBlocksGoodFor(-1); //do not cache
            return response;
        }

    } // namespace Methods
} // namespace RPC
