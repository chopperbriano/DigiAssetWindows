//
// Created by mctrivia on 17/03/24.
//
// RPC method "shutdown": exposed through the node's JSON-RPC server to bring the
// node to a safe stopping point. It halts the chain analyzer and the IPFS
// subsystem so no processing is left mid-write, then logs that it is safe for
// the operator to terminate the process.
//

#include "AppMain.h"
#include "Log.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Stops the node's background workers so the process can be terminated
        * cleanly. Calls stop() on the chain analyzer and on the IPFS controller,
        * then writes a CRITICAL "Safe to shut down" log line. params is ignored.
        * Returns true, uncached (blocksGoodFor = -1). Note: this does not exit the
        * process itself; the operator/host still terminates it.
        */
        extern const Response shutdown(const Json::Value& params) {
            AppMain* main=AppMain::GetInstance();
            main->getChainAnalyzer()->stop();
            main->getIPFS()->stop();
            Log* log = Log::GetInstance();
            log->addMessage("Safe to shut down", Log::CRITICAL);

            //return response
            Response response;
            response.setResult(true);
            response.setBlocksGoodFor(-1); //do not cache
            return response;
        }

    }
}