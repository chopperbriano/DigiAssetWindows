/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    abstractserverconnector.h
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: declares AbstractServerConnector, the base
// interface for the RPC transport that receives request bodies and returns
// responses for the node's/pool's JSON-RPC endpoint. Concrete connectors
// implement StartListening/StopListening for their transport; this base wires
// the connector to the protocol handler that processes each request.

#ifndef JSONRPC_CPP_SERVERCONNECTOR_H_
#define JSONRPC_CPP_SERVERCONNECTOR_H_

#include "iclientconnectionhandler.h"
#include <string>

namespace jsonrpc {

  // Transport base class: bridges an incoming request to a bound protocol
  // handler. Subclasses provide the concrete listen/accept mechanism.
  class AbstractServerConnector {
  public:
    AbstractServerConnector();
    virtual ~AbstractServerConnector();

    /**
     * This method should signal the Connector to start waiting for requests, in
     * any way that is appropriate for the derived connector class.
     * If something went wrong, this method should return false, otherwise true.
     */
    virtual bool StartListening() = 0;
    /**
     * This method should signal the Connector to stop waiting for requests, in
     * any way that is appropriate for the derived connector class.
     * If something went wrong, this method should return false, otherwise true.
     */
    virtual bool StopListening() = 0;

    // Forward a received request body to the bound handler and return its
    // serialized response; no-op if no handler is set.
    void ProcessRequest(const std::string &request, std::string &response);

    void SetHandler(IClientConnectionHandler *handler);
    IClientConnectionHandler *GetHandler();

  private:
    IClientConnectionHandler *handler;
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_ERVERCONNECTOR_H_ */
