/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    rpcprotocolserver12.h
 * @date    10/25/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: part of the bundled libjson-rpc-cpp server
// library used by the node and pool server. Declares the combined v1+v2
// protocol handler that dispatches each request to the appropriate JSON-RPC
// version at runtime.

#ifndef JSONRPC_RPCPROTOCOLSERVER12_H
#define JSONRPC_RPCPROTOCOLSERVER12_H

#include "abstractprotocolhandler.h"
#include "rpcprotocolserverv1.h"
#include "rpcprotocolserverv2.h"

namespace jsonrpc {

  // Protocol handler that owns one v1 and one v2 handler and forwards each
  // request to whichever matches. Used when a server is configured for
  // JSONRPC_SERVER_V1V2. Method contracts are documented in the .cpp.
  class RpcProtocolServer12 : public IProtocolHandler {
  public:
    RpcProtocolServer12(IProcedureInvokationHandler &handler);

    void AddProcedure(const Procedure &procedure);
    void HandleRequest(const std::string &request, std::string &retValue);

  private:
    RpcProtocolServerV1 rpc1;
    RpcProtocolServerV2 rpc2;

    // Selects rpc1 or rpc2 based on the shape/fields of the parsed request.
    AbstractProtocolHandler &GetHandler(const Json::Value &request);
  };

} // namespace jsonrpc

#endif // JSONRPC_RPCPROTOCOLSERVER12_H
