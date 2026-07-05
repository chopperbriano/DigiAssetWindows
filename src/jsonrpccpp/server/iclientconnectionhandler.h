/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    iclientconnectionhandler.h
 * @date    10/23/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Role in DigiAsset for Windows:
 * Defines the small abstract interfaces that decouple the JSON-RPC server
 * connectors (TCP / Unix-domain socket, HTTP, etc.) from the RPC processing
 * logic. A connector holds an IClientConnectionHandler and calls HandleRequest
 * to turn a raw request string into a response; IProtocolHandler extends it
 * with procedure registration. Part of the bundled libjson-rpc-cpp dependency;
 * header-only interface definitions.
 */

#ifndef JSONRPC_CPP_ICLIENTCONNECTIONHANDLER_H
#define JSONRPC_CPP_ICLIENTCONNECTIONHANDLER_H

#include <string>

namespace jsonrpc {
  class Procedure;

  /**
   * @brief Interface a connector uses to process one client request.
   *
   * Implementations turn a raw request string into a response string.
   */
  class IClientConnectionHandler {
  public:
    virtual ~IClientConnectionHandler() {}

    /**
     * @brief Processes a request and produces its response.
     * @param request The raw incoming request payload.
     * @param retValue Out-param filled with the response to send back.
     */
    virtual void HandleRequest(const std::string &request, std::string &retValue) = 0;
  };

  /**
   * @brief A connection handler that also supports registering RPC procedures.
   */
  class IProtocolHandler : public IClientConnectionHandler {
  public:
    virtual ~IProtocolHandler() {}

    /**
     * @brief Registers a procedure this handler will accept and dispatch.
     * @param procedure The procedure definition to add.
     */
    virtual void AddProcedure(const Procedure &procedure) = 0;
  };
} // namespace jsonrpc

#endif // JSONRPC_CPP_ICLIENTCONNECTIONHANDLER_H
