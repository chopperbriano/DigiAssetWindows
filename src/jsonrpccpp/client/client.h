/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    client.h
 * @date    03.01.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// File role: Declares jsonrpc::Client, the transport-agnostic JSON-RPC client
// core of libjson-rpc-cpp. In this repo it is the foundation for talking to the
// local DigiByte Core daemon over its RPC interface (a concrete IClientConnector
// such as HttpClient supplies the actual transport). The Client turns method
// names + JSON params into JSON-RPC request strings, hands them to the connector,
// and parses the JSON-RPC responses back into Json::Value results.

#ifndef JSONRPC_CPP_CLIENT_H_
#define JSONRPC_CPP_CLIENT_H_

#include "batchcall.h"
#include "batchresponse.h"
#include "iclientconnector.h"
#include <jsonrpccpp/common/jsonparser.h>

#include <map>
#include <vector>

namespace jsonrpc {
  class RpcProtocolClient;

  // Selects which JSON-RPC protocol version requests are built in / responses
  // parsed against (1.0 vs 2.0).
  typedef enum { JSONRPC_CLIENT_V1, JSONRPC_CLIENT_V2 } clientVersion_t;

  // Transport-agnostic JSON-RPC client. Holds a reference to a caller-owned
  // connector (the transport) and an owned RpcProtocolClient (the codec), and
  // exposes method calls, batch calls, and fire-and-forget notifications.
  class Client {
  public:
    // Builds a client over the given connector. version picks JSON-RPC 1/2;
    // omitEndingLineFeed suppresses the trailing newline on outgoing requests.
    // The connector reference must outlive the Client; the protocol object is
    // allocated here and freed by the destructor.
    Client(IClientConnector &connector, clientVersion_t version = JSONRPC_CLIENT_V2, bool omitEndingLineFeed = false);
    virtual ~Client();

    // Invokes a remote method and stores the parsed result into `result`.
    // Throws JsonRpcException on transport or protocol errors.
    void CallMethod(const std::string &name, const Json::Value &parameter, Json::Value &result);
    // Convenience overload of CallMethod that returns the result by value.
    Json::Value CallMethod(const std::string &name, const Json::Value &parameter);

    // Sends a batch of calls in a single request and fills `response` with the
    // per-call results/errors, matched by JSON-RPC id.
    void CallProcedures(const BatchCall &calls, BatchResponse &response);
    // Convenience overload of CallProcedures that returns the BatchResponse.
    BatchResponse CallProcedures(const BatchCall &calls);

    // Sends a JSON-RPC notification (no id, no response expected/parsed).
    void CallNotification(const std::string &name, const Json::Value &parameter);

  private:
    IClientConnector &connector;
    RpcProtocolClient *protocol;
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_CLIENT_H_ */
