//
// Created by DigiAsset Core on 14/07/26.
//

#include "AppMain.h"
#include "AssetWallet.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include "utils.h"
#include <jsoncpp/json/value.h>
#include <map>
#include <set>
#include <string>

namespace RPC {
    namespace Methods {
        /**
        * Returns the wallet's spendable DigiByte balance and all DigiAssets it holds.
        *
        * params[0] - minconf(integer optional default 1) - minimum confirmations a UTXO needs to be counted
        *
        * @return object:
        *   "digibyte"(object):
        *       "sats"(integer) - spendable DigiByte in sats(includes DigiByte locked in asset UTXOs)
        *       "amount"(string) - same value in decimal DGB
        *   "digidollar"(object):
        *       "cents"(integer) - DigiDollar held, in cents(100 == $1.00)
        *       "amount"(string) - same value in decimal dollars
        *   "assets"(array) - one entry per distinct asset held:
        *       "assetIndex"(integer)
        *       "assetId"(string)
        *       "cid"(string) - IPFS cid of the asset's metadata
        *       "decimals"(integer)
        *       "count"(integer) - amount held in smallest divisible units
        *       "amount"(string) - amount held in display units
        *
        * Use getassetdata with an assetIndex to look up an asset's name and other metadata.
        */
        extern const Response getwalletbalances(const Json::Value& params) {
            if (params.size() > 1) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            int minconf = 1;
            if (params.size() == 1) {
                if (!params[0].isInt() || (params[0].asInt() < 0)) {
                    throw DigiByteException(RPC_INVALID_PARAMS, "minconf must be a non negative integer");
                }
                minconf = params[0].asInt();
            }

            //add up everything in the wallet
            uint64_t digibyte = 0;
            uint64_t digidollar = 0;
            std::map<uint64_t, uint64_t> assetCounts; //assetIndex -> count
            std::map<uint64_t, DigiAsset> assetData;  //assetIndex -> asset(for id/cid/decimals)
            Database* db = AppMain::GetInstance()->getDatabase();
            std::set<std::string> countedAddresses; //a DigiDollar balance is per address, not per utxo
            for (const AssetUTXO& utxo: AssetWallet::getWalletUTXOs(minconf)) {
                digibyte += utxo.digibyte;
                for (const DigiAsset& asset: utxo.assets) {
                    uint64_t assetIndex = asset.getAssetIndex();
                    assetCounts[assetIndex] += asset.getCount();
                    if (assetData.count(assetIndex) == 0) assetData[assetIndex] = asset;
                }

                //DigiDollar lives on its own 0 value taproot outputs, which the wallet may or may
                //not return here, so it is looked up per address rather than read off the utxo.
                //Each address is only counted once no matter how many utxos it appears on.
                if (utxo.address.empty()) continue;
                if (!countedAddresses.insert(utxo.address).second) continue;
                digidollar += db->getDigiDollarBalance(utxo.address);
            }

            //convert to json
            Json::Value digibyteResult = Json::objectValue;
            digibyteResult["sats"] = static_cast<Json::UInt64>(digibyte);
            digibyteResult["amount"] = AssetWallet::satsToDecimal(digibyte);

            Json::Value assets = Json::arrayValue;
            for (const auto& entry: assetCounts) {
                const DigiAsset& asset = assetData[entry.first];
                Json::Value assetResult = Json::objectValue;
                assetResult["assetIndex"] = static_cast<Json::UInt64>(entry.first);
                assetResult["assetId"] = asset.getAssetId();
                assetResult["cid"] = asset.getCID();
                assetResult["decimals"] = asset.getDecimals();
                assetResult["count"] = static_cast<Json::UInt64>(entry.second);

                //display amount(count adjusted for decimals)
                uint64_t multiplier = BitIO::pow10(asset.getDecimals());
                std::string amount = std::to_string(entry.second / multiplier);
                if (asset.getDecimals() > 0) {
                    std::string frac = std::to_string(entry.second % multiplier);
                    while (frac.length() < asset.getDecimals()) frac.insert(frac.begin(), '0');
                    amount += "." + frac;
                }
                assetResult["amount"] = amount;
                assets.append(assetResult);
            }

            //DigiDollar is not a DigiAsset so it gets its own top level key rather than an entry
            //in "assets".  cents is the protocol's own unit; "amount" is display only.
            Json::Value digidollarResult = Json::objectValue;
            digidollarResult["cents"] = static_cast<Json::UInt64>(digidollar);
            digidollarResult["amount"] = utils::toDecimalString(digidollar, 2);

            Json::Value result = Json::objectValue;
            result["digibyte"] = digibyteResult;
            result["digidollar"] = digidollarResult;
            result["assets"] = assets;

            //return response
            Response response;
            response.setResult(result);
            response.setBlocksGoodFor(-1); //wallet balances can change every block so don't cache
            return response;
        }

    } // namespace Methods
} // namespace RPC
