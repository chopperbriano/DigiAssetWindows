/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    requesthandlerfactory.h
 * @date    10/23/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: part of the bundled libjson-rpc-cpp server
// library used by the node and pool server. Declares the factory that maps a
// requested JSON-RPC server version to the matching protocol handler class.

#ifndef JSONRPC_REQUESTHANDLERFACTORY_H
#define JSONRPC_REQUESTHANDLERFACTORY_H

#include "iclientconnectionhandler.h"
#include "iprocedureinvokationhandler.h"

namespace jsonrpc {

  // Which JSON-RPC dialect a server speaks: v1 only, v2 only, or both v1 and v2
  // auto-detected per request.
  typedef enum { JSONRPC_SERVER_V1, JSONRPC_SERVER_V2, JSONRPC_SERVER_V1V2 } serverVersion_t;

  // Stateless factory that builds the correct IProtocolHandler for a server
  // version.
  class RequestHandlerFactory {
  public:
    // See requesthandlerfactory.cpp: creates the handler for the given version,
    // wiring in handler as the procedure-invokation sink.
    static IProtocolHandler *createProtocolHandler(serverVersion_t type, IProcedureInvokationHandler &handler);
  };

} // namespace jsonrpc

#endif // JSONRPC_REQUESTHANDLERFACTORY_H
