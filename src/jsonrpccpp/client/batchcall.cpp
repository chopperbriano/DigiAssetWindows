/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    batchcall.cpp
 * @date    15.10.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Implementation of BatchCall: accumulates several JSON-RPC 2.0 requests into a
// single JSON array so the node/pool can send many Core RPC calls in one HTTP
// round-trip and serialize them for transport.

#include "batchcall.h"
#include "rpcprotocolclient.h"

using namespace jsonrpc;
using namespace std;

// Start id counter at 1; each non-notification call gets the next id.
BatchCall::BatchCall() : id(1) {}

// Append one JSON-RPC 2.0 request object (jsonrpc, method, optional params) to
// the batch array. Notifications get no id and return -1; regular calls are
// assigned and return the next sequential id for later result lookup.
// Note: params are only attached when null or non-empty (an empty container is
// treated as "no params" and omitted).

int BatchCall::addCall(const string &methodname, const Json::Value &params, bool isNotification) {
  Json::Value call;
  call[RpcProtocolClient::KEY_PROTOCOL_VERSION] = "2.0";
  call[RpcProtocolClient::KEY_PROCEDURE_NAME] = methodname;

  if (params.isNull() || !params.empty())
    call[RpcProtocolClient::KEY_PARAMETER] = params;

  if (!isNotification) {
    call[RpcProtocolClient::KEY_ID] = this->id++;
  }
  result.append(call);

  if (isNotification)
    return -1;
  return call[RpcProtocolClient::KEY_ID].asInt();
}

// Serialize the accumulated batch array to a JSON string. fast=true emits
// compact output (no indentation); fast=false uses the writer's default
// pretty formatting. Returns the JSON text to place in the HTTP request body.
string BatchCall::toString(bool fast) const {
  string result;
  if (fast) {
    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "";
    result = Json::writeString(wbuilder, this->result);
  } else {
    Json::StreamWriterBuilder wbuilder;
    result = Json::writeString(wbuilder, this->result);
  }
  return result;
}
