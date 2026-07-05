/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    rpcprotocolserverv1.h
 * @date    10/23/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Part of the bundled libjson-rpc-cpp library used by this node/pool to expose
// its JSON-RPC API. This header declares the server-side handler for the
// JSON-RPC 1.0 wire protocol: it parses/validates incoming request objects,
// dispatches them to the procedure invocation handler, and shapes the reply
// (result, error, or exception) into the 1.0 response format. See
// rpcprotocolserverv2.* for the JSON-RPC 2.0 counterpart.

#ifndef JSONRPC_CPP_RPCPROTOCOLSERVERV1_H
#define JSONRPC_CPP_RPCPROTOCOLSERVERV1_H

#include "abstractprotocolhandler.h"
#include <jsonrpccpp/common/exception.h>

namespace jsonrpc {

  // Concrete AbstractProtocolHandler implementing the JSON-RPC 1.0 protocol.
  // Given a decoded JSON request it validates the required fields, invokes the
  // target procedure through the supplied IProcedureInvokationHandler, and wraps
  // the outcome as a 1.0-format response.
  class RpcProtocolServerV1 : public AbstractProtocolHandler {
  public:
    RpcProtocolServerV1(IProcedureInvokationHandler &handler);

    // Returns true when the request is a well-formed JSON-RPC 1.0 call
    // (correct method-name and id/params field shapes), false otherwise.
    bool ValidateRequestFields(const Json::Value &request);
    // Entry point: validates the request, runs the procedure, and fills
    // response with the 1.0 result or error object.
    void HandleJsonRequest(const Json::Value &request, Json::Value &response);
    // Builds a successful 1.0 response into response from the procedure's
    // return value (result set, error null, matching id).
    void WrapResult(const Json::Value &request, Json::Value &response, Json::Value &retValue);
    // Builds a 1.0 error response into result carrying the given code/message.
    void WrapError(const Json::Value &request, int code, const std::string &message, Json::Value &result);
    // Turns a thrown JsonRpcException into a 1.0 error response in result.
    void WrapException(const Json::Value &request, const JsonRpcException &exception, Json::Value &result);
    // Classifies the request as a method call (has id) or a notification.
    procedure_t GetRequestType(const Json::Value &request);
  };

} // namespace jsonrpc

#endif // JSONRPC_CPP_RPCPROTOCOLSERVERV1_H
