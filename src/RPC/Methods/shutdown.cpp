//
// Created by mctrivia on 17/03/24.
//
// RPC method "shutdown": exposed through the node's JSON-RPC server to stop the
// node cleanly. It replies at once and signals the main thread, which stops every
// subsystem in order, flushes chain.db and exits the process.
//

#include "AppMain.h"
#include "Log.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <csignal>
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Requests a clean shutdown: logs it and raises SIGTERM, which the main thread
        * turns into the ordered teardown (RPC server, web console, chain analyzer,
        * pool threads, IPFS, WAL flush) and a process exit. params is ignored.
        * Returns true at once, uncached (blocksGoodFor = -1); callers wait for the
        * process to exit, which can take a minute while the analyzer finishes its block.
        */
        extern const Response shutdown(const Json::Value& params) {
            //Hand the whole shutdown to the main thread - the same path as ctrl-c, which
            //already stops the RPC server, web console, chain analyzer, pool threads and
            //IPFS in a safe order and then flushes the WAL.  This used to stop the analyzer
            //and IPFS here first, on the RPC thread, before replying: the analyzer finishes
            //its current block, so the reply routinely outlasted the CLI's 10 s timeout and
            //callers saw "libcurl error: 22" (a timeout in the node's curl numbering) for a
            //shutdown that was in fact under way.  The response still goes out because the
            //RPC worker pool is drained before the sockets close.
            Log* log = Log::GetInstance();
            log->addMessage("Shutdown requested over RPC - stopping cleanly", Log::CRITICAL);
            std::raise(SIGTERM);

            //return response
            Response response;
            response.setResult(true);
            response.setBlocksGoodFor(-1); //do not cache
            return response;
        }

    }
}