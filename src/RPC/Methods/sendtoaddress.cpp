//
// Created by mctrivia on 17/03/24.
//
// RPC method "sendtoaddress": a thin wrapper around DigiByte Core's wallet
// sendtoaddress command, exposed through the node's JSON-RPC server. Its only
// added behaviour over the standard Bitcoin/DigiByte call is resolving a
// DigiByte Domain in the destination field to a real address before forwarding.
//
#include "AppMain.h"
#include "DigiByteDomain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * params - see https://developer.bitcoin.org/reference/rpc/sendtoaddress.html
        * only difference is we now accept domains
        *
        * Validates the param count (2..9) and that the first param (destination)
        * is a string. If that destination is a DigiByte Domain it is rewritten to
        * the address it points to, then the (possibly modified) params are passed
        * straight through to DigiByte Core's wallet via sendcommand. The wallet's
        * result (the txid on success) is returned uncached (blocksGoodFor = -1).
        * Throws DigiByteException(RPC_INVALID_PARAMS) on bad input.
        */
        extern const Response sendtoaddress(const Json::Value& params) {
            if (params.size() < 2 || params.size() > 9) {
                throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            }
            if (!params[0].isString()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");

            //check if any domains in outputs
            std::vector<std::string> keysToRemove;
            Value newParams = params;
            if (DigiByteDomain::isDomain(newParams[0].asString())) {
                //change the domain into an address
                newParams[0] = DigiByteDomain::getAddress(newParams[0].asString());
            }

            //send modified params to wallet
            Json::Value result= AppMain::GetInstance()->getDigiByteCore()->sendcommand("sendtoaddress", newParams);

            //return response
            Response response;
            response.setResult(result);
            response.setBlocksGoodFor(-1); //do not cache
            return response;
        }

    }
}