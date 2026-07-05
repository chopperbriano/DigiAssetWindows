//
// Created by mctrivia on 17/03/24.
//
// getipfscount.cpp - RPC method implementation for the node
// (DigiAssetWindows.exe). Registered in the RPC::Methods dispatch table and
// exposed over the JSON-RPC server. Reports how many IPFS jobs are currently
// queued in the local database, letting operators gauge the node's pending
// pin/fetch backlog. Result is never cached.
//

#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Returns the number of IPFS jobs in que
        */
        extern const Response getipfscount(const Json::Value& params) {
            if (params.size() != 0) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            Database* db = AppMain::GetInstance()->getDatabase();

            //return response
            Response response;
            response.setResult(db->getIPFSJobCount());
            response.setBlocksGoodFor(-1); //do not cache
            return response;
        }

    }
}