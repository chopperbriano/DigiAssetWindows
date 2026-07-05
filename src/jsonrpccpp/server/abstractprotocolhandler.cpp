/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    abstractprotocolhandler.cpp
 * @date    10/23/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: shared base for the JSON-RPC protocol
// handlers used by the node's/pool's RPC server. It owns the registry of
// callable procedures and turns raw request text into parsed JSON, validated
// dispatch, and serialized response text. Version-specific concerns (JSON-RPC
// v1 vs v2 envelope shape, error/result wrapping) are left to derived classes
// via the pure-virtual hooks declared in the header.

#include "abstractprotocolhandler.h"
#include <jsonrpccpp/common/errors.h>
#include <sstream>
#include <map>

using namespace jsonrpc;
using namespace std;

// Store the invocation handler that will actually run the bound C++ methods
// (typically the AbstractServer subclass) once a request is dispatched.
AbstractProtocolHandler::AbstractProtocolHandler(IProcedureInvokationHandler &handler) : handler(handler) {}

AbstractProtocolHandler::~AbstractProtocolHandler() {}

// Register a procedure (method or notification) under its name so it can be
// looked up during validation and dispatch.
void AbstractProtocolHandler::AddProcedure(const Procedure &procedure) { this->procedures[procedure.GetProcedureName()] = procedure; }

// Entry point from the server connector: parse the raw request string as JSON,
// dispatch it via HandleJsonRequest, and serialize the response back into
// retValue. A JSON parse failure is turned into a JSON-RPC parse-error
// response. A null response (e.g. a notification) leaves retValue untouched.
void AbstractProtocolHandler::HandleRequest(const std::string &request, std::string &retValue) {
  Json::Value req;
  Json::Value resp;
  Json::StreamWriterBuilder wbuilder;
  wbuilder["indentation"] = "";

  try {
    istringstream(request) >> req;
    this->HandleJsonRequest(req, resp);
  } catch (const Json::Exception &e) {
    this->WrapError(Json::nullValue, Errors::ERROR_RPC_JSON_PARSE_ERROR, Errors::GetErrorMessage(Errors::ERROR_RPC_JSON_PARSE_ERROR), resp);
  }

  if (resp != Json::nullValue)
    retValue = Json::writeString(wbuilder, resp);
}

// Dispatch a single already-validated request to its bound handler. Methods
// run via HandleMethodCall and have their return value wrapped into response;
// notifications run via HandleNotificationCall and produce a null response
// (no reply is sent for notifications).
void AbstractProtocolHandler::ProcessRequest(const Json::Value &request, Json::Value &response) {
  Procedure &method = this->procedures[request[KEY_REQUEST_METHODNAME].asString()];
  Json::Value result;

  if (method.GetProcedureType() == RPC_METHOD) {
    handler.HandleMethodCall(method, request[KEY_REQUEST_PARAMETERS], result);
    this->WrapResult(request, response, result);
  } else {
    handler.HandleNotificationCall(method, request[KEY_REQUEST_PARAMETERS]);
    response = Json::nullValue;
  }
}

// Check a parsed request before dispatch and return 0 if valid, otherwise a
// JSON-RPC error code: invalid request shape, unknown method, a mismatch
// between the request type and the registered procedure type (method called as
// notification or vice versa), or parameters that fail the procedure's schema.
int AbstractProtocolHandler::ValidateRequest(const Json::Value &request) {
  int error = 0;
  Procedure proc;
  if (!this->ValidateRequestFields(request)) {
    error = Errors::ERROR_RPC_INVALID_REQUEST;
  } else {
    map<string, Procedure>::iterator it = this->procedures.find(request[KEY_REQUEST_METHODNAME].asString());
    if (it != this->procedures.end()) {
      proc = it->second;
      if (this->GetRequestType(request) == RPC_METHOD && proc.GetProcedureType() == RPC_NOTIFICATION) {
        error = Errors::ERROR_SERVER_PROCEDURE_IS_NOTIFICATION;
      } else if (this->GetRequestType(request) == RPC_NOTIFICATION && proc.GetProcedureType() == RPC_METHOD) {
        error = Errors::ERROR_SERVER_PROCEDURE_IS_METHOD;
      } else if (!proc.ValdiateParameters(request[KEY_REQUEST_PARAMETERS])) {
        error = Errors::ERROR_RPC_INVALID_PARAMS;
      }
    } else {
      error = Errors::ERROR_RPC_METHOD_NOT_FOUND;
    }
  }
  return error;
}
