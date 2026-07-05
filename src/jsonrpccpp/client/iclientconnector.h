/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    abstractclientconnector.h
 * @date    02.01.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in the node/pool: defines the transport-agnostic interface that the
// JSON-RPC client layer uses to ship a request and receive a response. Concrete
// transports (e.g. WindowsTcpSocketClient talking to the DigiByte daemon)
// implement this so the client code stays independent of the underlying socket.

#ifndef JSONRPC_CPP_CLIENTCONNECTOR_H_
#define JSONRPC_CPP_CLIENTCONNECTOR_H_

#include <jsonrpccpp/common/exception.h>
#include <string>

namespace jsonrpc {
  /**
   * @brief Abstract base for JSON-RPC client transports.
   *
   * A connector knows only how to send an already-serialized request string and
   * return the raw response string; request/response encoding is handled
   * elsewhere (RpcProtocolClient). Implementations own the actual channel.
   */
  class IClientConnector {
  public:
    virtual ~IClientConnector() {}

    /**
     * @brief Transmits a serialized JSON-RPC message and collects the reply.
     * @param message The serialized request to send.
     * @param result  Receives the raw response returned by the server.
     */
    virtual void SendRPCMessage(const std::string &message, std::string &result) = 0;
  };
} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_CLIENTCONNECTOR_H_ */
