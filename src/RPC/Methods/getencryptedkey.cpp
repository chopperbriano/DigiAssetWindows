//
// Created by mctrivia on 09/07/24.
//
// RPC method "getencryptedkey": implements the JSON-RPC handler that returns the
// encrypted key blob(s) the node has stored for one or more addresses, hex-encoded.
// Stops at the first address that has no stored value and records the remaining
// (unused) addresses so the cache layer can invalidate the result once one of them
// changes on chain. Registered with the node's RPC server; part of the node
// deployable (DigiAssetWindows.exe).
//

#include "AppMain.h"
#include "DigiByteDomain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Return encrypted data associated with an address or list of addresses
        *  params[0] - address(string, or array of strings)
        */
        extern const Response getencryptedkey(const Json::Value& params) {
            if (params.size() != 1) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");

            //make input an array of strings
            vector<string> addresses;
            if (params[0].isString()) {
                addresses.emplace_back(params[0].asString());
            } else if (params[0].isArray()) {
                for (const auto& address: params[0]) {
                    if (!address.isString()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                    addresses.emplace_back(address.asString());
                }
            } else {
                throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            }

            //process addresses until you find one that wasn't used
            Json::Value json = Json::arrayValue;
            vector<string> unusedAddresses;
            size_t i;
            try {

                //get list of values stored in database
                Database* db = AppMain::GetInstance()->getDatabase();
                for (i = 0; i < addresses.size(); i++) {
                    json.append(db->getEncryptedKey(addresses[i]).toHex());
                }

            } catch (const Database::exceptionFailedSelect& e) {

                //mark what addresses where unused so caching system can no when results changed
                for (; i < addresses.size(); i++) {
                    unusedAddresses.emplace_back(addresses[i]);
                }
            }

            //return response
            Response response;
            response.setResult(json);
            response.setBlocksGoodFor(5760); //day
            for (const auto& address: unusedAddresses) {
                response.addInvalidateOnAddressChange(address); //result good until unused addresses changed
            }
            return response;
        }

    } // namespace Methods
} // namespace RPC