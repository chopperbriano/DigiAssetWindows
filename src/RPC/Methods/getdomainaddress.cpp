//
// Created by mctrivia on 17/03/24.
//
// RPC method "getdomainaddress": implements the JSON-RPC handler that resolves a
// DigiByte-Domain name to the DigiByte address that currently controls it, delegating
// the lookup to DigiByteDomain. Reports distinct errors for unknown vs. revoked
// domains. Registered with the node's RPC server; part of the node deployable
// (DigiAssetWindows.exe).
//

#include "AppMain.h"
#include "DigiByteDomain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Returns the DigiByte address currently associated with a domain
        *  params[0] - domain(string)
        */
        extern const Response getdomainaddress(const Json::Value& params) {
            if (params.size() != 1) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            if (!params[0].isString()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            try {
                string address=DigiByteDomain::getAddress(params[0].asString());

                //return response
                Response response;
                response.setResult(address);
                response.setBlocksGoodFor(5760); //day
                response.addInvalidateOnAddressChange(address);//result good until address moves the asset
                return response;
            } catch (const DigiByteDomain::exceptionUnknownDomain& e) {
                throw DigiByteException(RPC_MISC_ERROR, "Unknown Domain");
            } catch (const DigiByteDomain::exceptionRevokedDomain& e) {
                throw DigiByteException(RPC_MISC_ERROR, "Domain Revoked");
            } catch (const DigiByteDomain::exceptionBurnedDomain& e) {
                // Domain is registered but its controlling asset has no holders
                // (swept by a non-DigiAsset-aware wallet). Distinct from a node
                // outage — surface it so clients can say "burned", not "couldn't
                // check". Without this catch it escapes to the server's generic
                // handler and is reported as "Unexpected Error".
                throw DigiByteException(RPC_MISC_ERROR, "Domain Burned");
            }
        }

    }
}