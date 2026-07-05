//
// Created by mctrivia on 17/03/24.
//
// RPC method "version": exposed through the node's JSON-RPC server. Returns the
// running build's version string (from Version.h) so clients can confirm which
// node release they are talking to.
//

#include "Version.h"
#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Returns the current version number (getVersionString()). params is
        * ignored. The result is cacheable for 5760 blocks (roughly one day) since
        * the version does not change while the node runs.
        */
        extern const Response version(const Json::Value& params) {
            //return response
            Response response;
            response.setResult(getVersionString());
            response.setBlocksGoodFor(5760); //day
            return response;
        }

    }
}