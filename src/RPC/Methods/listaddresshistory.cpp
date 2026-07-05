//
// Created by mctrivia on 17/03/24.
//
// RPC method "listaddresshistory" for the DigiAsset node's JSON-RPC server.
// A node-specific (non-Core) method: queries the node's Database for the list of
// txids that a given address participated in, filtered by block-height range and
// count limit. Availability of results depends on the node's pruning level.
// Registered in RPC/Server and dispatched by the RPC handler.
//

#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Returns a list of txids that an address was involved in
        *
        * warning results will be limited based on pruning level
        *
        * params[0] - address(string)
        * params[1] - minHeight(optional unsigned int default 1)
        * params[2] - maxHeight(optional unsigned int default infinity)
        * params[3] - limit(optional unsigned int default infinity)
        */
        extern const Response listaddresshistory(const Json::Value& params) {
            //get paramas
            if ( (params.size() < 1) || (params.size()>4) ) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            if (!params[0].isString()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            string address=params[0].asString();
            unsigned int minHeight=1;
            if ( (params.size()>1) && (!params[1].isNull()) ) {
                if (!params[1].isUInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                minHeight=params[1].asUInt();
            }
            unsigned int maxHeight=std::numeric_limits<unsigned int>::max();
            if ( (params.size()>2) && (!params[2].isNull()) ) {
                if (!params[2].isUInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                maxHeight=params[2].asUInt();
            }
            if (maxHeight<minHeight) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            unsigned int limit=std::numeric_limits<unsigned int>::max();
            if (params.size()>3) {
                if (!params[3].isUInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                limit=params[3].asUInt();
            }

            //lookup transactions in database
            Database* db=AppMain::GetInstance()->getDatabase();
            auto txList=db->getAddressTxList(address,minHeight,maxHeight,limit);

            //convert to json
            Json::Value result=Json::arrayValue;
            for (const string& tx : txList) {
                result.append(tx);
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