/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    rpcprotocolserver12.cpp
 * @date    10/25/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: part of the bundled libjson-rpc-cpp server
// library used by the node and pool server. Implements the combined v1+v2
// protocol handler, which sniffs each incoming request and routes it to either
// the JSON-RPC 1.0 or the 2.0 handler so a single endpoint can serve both.

#include "rpcprotocolserver12.h"
#include <jsonrpccpp/common/jsonparser.h>

using namespace jsonrpc;
using namespace std;

// Build both sub-handlers over the same procedure-invokation sink so a request
// can be dispatched to whichever protocol version it matches.
RpcProtocolServer12::RpcProtocolServer12(IProcedureInvokationHandler &handler) : rpc1(handler), rpc2(handler) {}

// Register a callable procedure with both the v1 and v2 handlers so it is
// reachable regardless of which protocol the client uses.
void RpcProtocolServer12::AddProcedure(const Procedure &procedure) {
  this->rpc1.AddProcedure(procedure);
  this->rpc2.AddProcedure(procedure);
}

// Parse a raw request string, route it to the matching protocol handler, and
// serialize the reply into retValue. On a JSON parse failure, emits a JSON-RPC
// parse-error response. Leaves retValue untouched when the handler produces no
// response (e.g. a notification), so the caller can send an empty body.
void RpcProtocolServer12::HandleRequest(const std::string &request, std::string &retValue) {
  Json::Value req;
  Json::Value resp;
  Json::StreamWriterBuilder wbuilder;
  wbuilder["indentation"] = "";

  try {
    istringstream(request) >> req;
    this->GetHandler(req).HandleJsonRequest(req, resp);
  } catch (const Json::Exception &e) {
    this->GetHandler(req).WrapError(Json::nullValue, Errors::ERROR_RPC_JSON_PARSE_ERROR, Errors::GetErrorMessage(Errors::ERROR_RPC_JSON_PARSE_ERROR), resp);
  }

  if (resp != Json::nullValue)
    retValue = Json::writeString(wbuilder, resp);
}

// Pick the protocol handler for a parsed request: use the v2 handler for batch
// arrays or objects carrying "jsonrpc":"2.0", otherwise fall back to v1.
AbstractProtocolHandler &RpcProtocolServer12::GetHandler(const Json::Value &request) {
  if (request.isArray() || (request.isObject() && request.isMember("jsonrpc") && request["jsonrpc"].asString() == "2.0"))
    return rpc2;
  return rpc1;
}
