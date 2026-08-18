//
// Created by mctrivia on 26/03/24.
//
// RPC method handler: "getaddressholdings".
//
// Part of the node's JSON-RPC surface. Queries the chain-analysis database for
// all DigiAsset (and optionally DigiByte) balances currently held by a single
// address, returning them as a map of assetIndex -> quantity. Result is cached
// for ~1 day (5760 blocks) and invalidated whenever that address changes.
//

#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Returns address kyc information
        * params[0] - address(string)
        *
        * return an object with assetIndex as key and asset quantity as value.  Quantities are in
        * the asset's smallest unit - sats for assetIndex 1.
        * Please note assetIndex 1 is DigiByte and this will be included if storenonassetutxo=1 and
        * the address actually holds some.  Only unspent holdings are reported, and an index whose
        * unspent total is zero is omitted rather than reported as 0 - so an address holding only
        * DigiDollar has no "1" key even though its DigiDollar output technically sits in the utxo
        * table carrying 0 DigiByte.
        *
        * DigiDollar is not a DigiAsset and has no assetIndex, so it is reported under the separate
        * key "digidollar" holding the balance in cents(100 == $1.00).  The key is only present when
        * the address holds DigiDollar, so callers iterating this object must expect a non numeric
        * key or filter for it explicitly.
        */
        extern const Response getaddressholdings(const Json::Value& params) {
            if (params.size() != 1) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            if (!params[0].isString()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");

            string address=params[0].asString();

            //get desired exchange rates
            Database* db = AppMain::GetInstance()->getDatabase();
            auto data = db->getAddressHoldings(address);

            //convert to json
            Value result=Json::objectValue;
            for (const auto& entry: data) {
                result[to_string(entry.assetIndex)]=static_cast<Json::UInt64>(entry.count);
            }

            //add DigiDollar if the address holds any
            uint64_t digidollar = db->getDigiDollarBalance(address);
            if (digidollar > 0) {
                result["digidollar"]=static_cast<Json::UInt64>(digidollar);
            }

            //return response
            Response response;
            response.setResult(result);
            response.setBlocksGoodFor(5760); //day
            response.addInvalidateOnAddressChange(address);
            return response;
        }

    }
}