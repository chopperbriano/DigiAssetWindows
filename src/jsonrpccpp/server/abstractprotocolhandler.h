/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    abstractprotocolhandler.h
 * @date    10/23/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: declares AbstractProtocolHandler, the base
// class for the JSON-RPC request/response protocol used by the node's/pool's
// RPC interface. It holds the procedure registry and the request-handling
// pipeline (parse -> validate -> dispatch -> wrap), while delegating the
// version-specific envelope details to concrete v1/v2 subclasses through the
// pure-virtual hooks below. The KEY_* macros name the standard JSON-RPC fields.

#ifndef JSONRPC_CPP_ABSTRACTPROTOCOLHANDLER_H
#define JSONRPC_CPP_ABSTRACTPROTOCOLHANDLER_H

#include "iclientconnectionhandler.h"
#include "iprocedureinvokationhandler.h"
#include <jsonrpccpp/common/procedure.h>
#include <map>
#include <string>

#define KEY_REQUEST_METHODNAME "method"
#define KEY_REQUEST_ID "id"
#define KEY_REQUEST_PARAMETERS "params"
#define KEY_RESPONSE_ERROR "error"
#define KEY_RESPONSE_RESULT "result"

namespace jsonrpc {

  // Base protocol handler: owns the registered-procedure map and drives the
  // common request lifecycle. Subclasses supply the JSON-RPC-version-specific
  // behavior via the pure-virtual methods (request parsing/validation and
  // result/error envelope wrapping).
  class AbstractProtocolHandler : public IProtocolHandler {
  public:
    AbstractProtocolHandler(IProcedureInvokationHandler &handler);
    virtual ~AbstractProtocolHandler();

    // Parse raw request text, dispatch it, and serialize the response text.
    void HandleRequest(const std::string &request, std::string &retValue);

    // Register a procedure so it can be validated and dispatched by name.
    virtual void AddProcedure(const Procedure &procedure);

    // Version-specific hooks implemented by concrete subclasses:
    //   HandleJsonRequest   - dispatch a parsed request into a parsed response.
    //   ValidateRequestFields - check that the envelope has the required fields.
    //   WrapResult          - build a success response around a method result.
    //   WrapError           - build an error response with code and message.
    //   GetRequestType      - classify a request as method or notification.
    virtual void HandleJsonRequest(const Json::Value &request, Json::Value &response) = 0;
    virtual bool ValidateRequestFields(const Json::Value &val) = 0;
    virtual void WrapResult(const Json::Value &request, Json::Value &response, Json::Value &retValue) = 0;
    virtual void WrapError(const Json::Value &request, int code, const std::string &message, Json::Value &result) = 0;
    virtual procedure_t GetRequestType(const Json::Value &request) = 0;

  protected:
    IProcedureInvokationHandler &handler;      // runs the bound C++ methods
    std::map<std::string, Procedure> procedures; // name -> registered procedure

    // Dispatch an already-validated request to its bound handler.
    void ProcessRequest(const Json::Value &request, Json::Value &retValue);
    // Return 0 if the request is valid, otherwise a JSON-RPC error code.
    int ValidateRequest(const Json::Value &val);
  };

} // namespace jsonrpc

#endif // JSONRPC_CPP_ABSTRACTPROTOCOLHANDLER_H
