/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    requesthandlerfactory.cpp
 * @date    10/23/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: part of the bundled libjson-rpc-cpp server
// library used by the node and pool server. Implements the factory that picks
// the concrete JSON-RPC protocol handler (v1, v2, or combined v1+v2) matching
// the server's configured protocol version.

#include "requesthandlerfactory.h"
#include "rpcprotocolserver12.h"
#include "rpcprotocolserverv1.h"
#include "rpcprotocolserverv2.h"

using namespace jsonrpc;

// Construct and return the protocol handler for the requested server version.
// type selects the JSON-RPC dialect; handler is the invokation sink forwarded
// to the created handler so decoded calls reach the RPC method bodies. Returns
// a heap-allocated handler (caller owns it); NULL if type is unrecognized.
IProtocolHandler *RequestHandlerFactory::createProtocolHandler(serverVersion_t type, IProcedureInvokationHandler &handler) {
  IProtocolHandler *result = NULL;
  switch (type) {
  case JSONRPC_SERVER_V1:
    result = new RpcProtocolServerV1(handler);
    break;
  case JSONRPC_SERVER_V2:
    result = new RpcProtocolServerV2(handler);
    break;
  case JSONRPC_SERVER_V1V2:
    result = new RpcProtocolServer12(handler);
    break;
  }
  return result;
}
