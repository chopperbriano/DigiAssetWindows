/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    client.cpp
 * @date    03.01.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// File role: Implements jsonrpc::Client, the transport-agnostic JSON-RPC client
// used (via connectors like HttpClient) to drive the local DigiByte Core RPC
// interface. Each call delegates request building/response parsing to an owned
// RpcProtocolClient and the actual send/receive to the injected connector.

#include "client.h"
#include "rpcprotocolclient.h"
#include <sstream>

using namespace jsonrpc;
using namespace std;

// Stores the connector reference and allocates the protocol codec for the
// requested JSON-RPC version.
Client::Client(IClientConnector &connector, clientVersion_t version, bool omitEndingLineFeed) : connector(connector) {
  this->protocol = new RpcProtocolClient(version, omitEndingLineFeed);
}

Client::~Client() { delete this->protocol; }

// Builds a request for `name`/`parameter`, sends it over the connector, and
// parses the response into `result`. Throws JsonRpcException on error.
void Client::CallMethod(const std::string &name, const Json::Value &parameter, Json::Value &result) {
  std::string request, response;
  protocol->BuildRequest(name, parameter, request, false);
  connector.SendRPCMessage(request, response);
  protocol->HandleResponse(response, result);
}

// Serializes the batch to a single request and sends it, then parses the
// response, which must be a JSON array. Each array element is dispatched
// through the protocol handler and added to `result`; a per-element
// JsonRpcException is captured as an error response (keyed by its id, or -1 if
// absent) rather than aborting the whole batch. Throws JsonRpcException if the
// overall response is not a JSON array or an element is not an object.
void Client::CallProcedures(const BatchCall &calls, BatchResponse &result) {
  std::string request, response;
  request = calls.toString();
  connector.SendRPCMessage(request, response);
  Json::Value tmpresult;

  try {
    istringstream(response) >> tmpresult;
    if(!tmpresult.isArray()) {
      throw JsonRpcException(Errors::ERROR_CLIENT_INVALID_RESPONSE, "Array expected.");
    }
  } catch (const Json::Exception &e) {
    throw JsonRpcException(Errors::ERROR_RPC_JSON_PARSE_ERROR, Errors::GetErrorMessage(Errors::ERROR_RPC_JSON_PARSE_ERROR), response);
  }

  for (unsigned int i = 0; i < tmpresult.size(); i++) {
    if (tmpresult[i].isObject()) {
      Json::Value singleResult;
      try {
        Json::Value id = this->protocol->HandleResponse(tmpresult[i], singleResult);
        result.addResponse(id, singleResult, false);
      } catch (JsonRpcException &ex) {
        Json::Value id = -1;
        if (tmpresult[i].isMember("id"))
          id = tmpresult[i]["id"];
        result.addResponse(id, tmpresult[i]["error"], true);
      }
    } else
      throw JsonRpcException(Errors::ERROR_CLIENT_INVALID_RESPONSE, "Object in Array expected.");
  }
}

// Convenience overload returning the BatchResponse by value.
BatchResponse Client::CallProcedures(const BatchCall &calls) {
  BatchResponse result;
  this->CallProcedures(calls, result);
  return result;
}

// Convenience overload returning the method result by value.
Json::Value Client::CallMethod(const std::string &name, const Json::Value &parameter) {
  Json::Value result;
  this->CallMethod(name, parameter, result);
  return result;
}

// Builds and sends a notification (isNotification=true, so no id); the response
// is not read back. Throws JsonRpcException on transport error.
void Client::CallNotification(const std::string &name, const Json::Value &parameter) {
  std::string request, response;
  protocol->BuildRequest(name, parameter, request, true);
  connector.SendRPCMessage(request, response);
}
