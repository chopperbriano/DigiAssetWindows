/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    rpcprotocolserverv2.h
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Part of the bundled libjson-rpc-cpp library used by this node/pool to expose
// its JSON-RPC API. This header declares the server-side handler for the
// JSON-RPC 2.0 wire protocol: it validates incoming requests, supports both
// single calls and batch arrays, dispatches to the procedure invocation
// handler, and formats results/errors per the 2.0 spec. See
// rpcprotocolserverv1.* for the older 1.0 handler.

#ifndef JSONRPC_CPP_RPCPROTOCOLSERVERV2_H_
#define JSONRPC_CPP_RPCPROTOCOLSERVERV2_H_

#include <map>
#include <string>
#include <vector>

#include "abstractprotocolhandler.h"
#include <jsonrpccpp/common/exception.h>

#define KEY_REQUEST_VERSION "jsonrpc"
#define JSON_RPC_VERSION2 "2.0"

namespace jsonrpc {
  // Concrete AbstractProtocolHandler implementing the JSON-RPC 2.0 protocol,
  // including batch requests (arrays of calls) and notifications (calls with no
  // id, which produce no reply).
  class RpcProtocolServerV2 : public AbstractProtocolHandler {
  public:
    RpcProtocolServerV2(IProcedureInvokationHandler &handler);

    // Entry point: dispatches to single- or batch-request handling based on
    // whether the decoded request is an object or an array; rejects anything
    // else as an invalid request.
    void HandleJsonRequest(const Json::Value &request, Json::Value &response);
    // Returns true when val is a well-formed 2.0 request (string method, the
    // "jsonrpc":"2.0" tag, and valid id/params field types).
    bool ValidateRequestFields(const Json::Value &val);
    // Builds a successful 2.0 response into response (jsonrpc tag, result,
    // matching id).
    void WrapResult(const Json::Value &request, Json::Value &response, Json::Value &retValue);
    // Builds a 2.0 error response into result with the given code/message,
    // echoing the request id when present and valid.
    void WrapError(const Json::Value &request, int code, const std::string &message, Json::Value &result);
    // Turns a thrown JsonRpcException into a 2.0 error response, attaching its
    // data payload under error.data.
    void WrapException(const Json::Value &request, const JsonRpcException &exception, Json::Value &result);
    // Classifies the request as a method call (has id) or a notification.
    procedure_t GetRequestType(const Json::Value &request);

  private:
    // Validates and processes one request object, wrapping any thrown
    // JsonRpcException as an error response.
    void HandleSingleRequest(const Json::Value &request, Json::Value &response);
    // Processes each element of a batch array, appending non-null per-call
    // results to response; an empty batch is reported as an invalid request.
    void HandleBatchRequest(const Json::Value &requests, Json::Value &response);
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_RPCPROTOCOLSERVERV2_H_ */
