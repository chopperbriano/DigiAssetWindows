//
// Created by RenzoDD on 06/04/24.
//
// RPC method "listlastblocks".
// Node-side JSON-RPC handler that reads the most recently processed blocks from
// the node's Database (getLastBlocks) and returns them as JSON objects
// (height, hash, time, algo). Used by clients/dashboards to show chain-sync
// progress. Dispatched by the node's RPC server.

#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Returns a list the last blocks processed
        *  params[0] - limit(unsigned int) - number of records
        *  params[1] - start(optional unsigned int default newest block) - what block to start from
        *
        * @return array of unsigned ints
        */
        extern const Response listlastblocks(const Json::Value& params) {
            if (params.size() > 2) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            if (!params[0].isUInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");

            //get variables
            unsigned int limit=params[0].asUInt();
            unsigned int start=std::numeric_limits<unsigned int>::max();
            if (params.size()==2) {
                if (!params[1].isUInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                start=params[1].asUInt();
            }

            //look up blocks
            Database* db = AppMain::GetInstance()->getDatabase();
            std::vector<BlockBasics> results= db->getLastBlocks(limit,start);

            //convert to json
            Value jsonArray=Json::arrayValue;
            for (const auto& result : results) {
                Json::Value blockJSON(Json::objectValue);
                blockJSON["height"] = result.height;
                blockJSON["hash"] = result.hash;
                blockJSON["time"] = result.time;
                blockJSON["algo"] = result.algo;
                jsonArray.append(blockJSON);
            }

            //return response
            Response response;
            response.setResult(jsonArray);
            return response;
        }

    }
}