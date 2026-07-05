//
// Created by mctrivia on 17/03/24.
//
// RPC method "resyncmetadata".
// Node-side JSON-RPC handler that re-pins all IPFS metadata for every
// Permanent Storage Pool the node is subscribed to (calls repinAllFiles on
// each subscribed pool). Returns immediately with true; the actual downloads
// happen asynchronously in the background. Dispatched by the node's RPC server.

#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * pins all ipfs meta data
        * returns true - does not mean they are all downloaded yet.  Will likely take a while to finish
        */
        extern const Response resyncmetadata(const Json::Value& params) {
            AppMain* main = AppMain::GetInstance();
            PermanentStoragePoolList* pools = main->getPermanentStoragePoolList();
            for (const auto& pool: *pools) {
                if (!pool->subscribed()) continue;
                pool->repinAllFiles();
            }

            //return response
            Response response;
            response.setResult(true);
            response.setBlocksGoodFor(-1); //do not cache
            return response;
        }

    }
}