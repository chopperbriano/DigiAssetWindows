/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    rpcprotocolserverv1.cpp
 * @date    10/23/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: part of the bundled libjson-rpc-cpp server
// library used by the node and pool server. Implements the JSON-RPC 1.0
// protocol handler: it validates the request envelope, distinguishes methods
// from notifications, and formats results/errors in the v1 (method/params/id
// with a top-level error object) response shape.

#include "rpcprotocolserverv1.h"
#include <jsonrpccpp/common/errors.h>
#include <jsonrpccpp/common/exception.h>

using namespace jsonrpc;

RpcProtocolServerV1::RpcProtocolServerV1(IProcedureInvokationHandler &handler) : AbstractProtocolHandler(handler) {}

// Handle a single parsed v1 request object: validate it, run the procedure and
// on success write the reply into response; on validation failure or a thrown
// JsonRpcException, write the corresponding error response instead. A
// non-object request yields an "invalid request" error.
void RpcProtocolServerV1::HandleJsonRequest(const Json::Value &req, Json::Value &response) {
  if (req.isObject()) {
    int error = this->ValidateRequest(req);
    if (error == 0) {
      try {
        this->ProcessRequest(req, response);
      } catch (const JsonRpcException &exc) {
        this->WrapException(req, exc, response);
      }
    } else {
      this->WrapError(req, error, Errors::GetErrorMessage(error), response);
    }
  } else {
    this->WrapError(Json::nullValue, Errors::ERROR_RPC_INVALID_REQUEST, Errors::GetErrorMessage(Errors::ERROR_RPC_INVALID_REQUEST), response);
  }
}

// Check that a request has the fields required by JSON-RPC 1.0: a string
// method name, an id member, and a params member that is either an array or
// null. Returns true only if all are present and well-typed.
bool RpcProtocolServerV1::ValidateRequestFields(const Json::Value &request) {
  if (!(request.isMember(KEY_REQUEST_METHODNAME) && request[KEY_REQUEST_METHODNAME].isString()))
    return false;
  if (!request.isMember(KEY_REQUEST_ID))
    return false;
  if (!request.isMember(KEY_REQUEST_PARAMETERS))
    return false;
  if (!(request[KEY_REQUEST_PARAMETERS].isArray() || request[KEY_REQUEST_PARAMETERS].isNull()))
    return false;
  return true;
}

// Build a v1 success response: set result to the procedure's return value, set
// error to null, and echo the request id.
void RpcProtocolServerV1::WrapResult(const Json::Value &request, Json::Value &response, Json::Value &retValue) {
  response[KEY_RESPONSE_RESULT] = retValue;
  response[KEY_RESPONSE_ERROR] = Json::nullValue;
  response[KEY_REQUEST_ID] = request[KEY_REQUEST_ID];
}

// Build a v1 error response: fill error.code and error.message, set result to
// null, and echo the request id when present (otherwise null).
void RpcProtocolServerV1::WrapError(const Json::Value &request, int code, const std::string &message, Json::Value &result) {
  result["error"]["code"] = code;
  result["error"]["message"] = message;
  result["result"] = Json::nullValue;
  if (request.isObject() && request.isMember("id")) {
    result["id"] = request["id"];
  } else {
    result["id"] = Json::nullValue;
  }
}

// Build a v1 error response from a thrown JsonRpcException, additionally
// attaching the exception's data payload under error.data.
void RpcProtocolServerV1::WrapException(const Json::Value &request, const JsonRpcException &exception, Json::Value &result) {
  this->WrapError(request, exception.GetCode(), exception.GetMessage(), result);
  result["error"]["data"] = exception.GetData();
}

// Classify a request as a notification (id is null) or a method call
// (otherwise), which decides whether a response is expected.
procedure_t RpcProtocolServerV1::GetRequestType(const Json::Value &request) {
  if (request[KEY_REQUEST_ID] == Json::nullValue)
    return RPC_NOTIFICATION;
  return RPC_METHOD;
}
