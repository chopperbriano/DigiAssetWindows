/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    responsehandler.cpp
 * @date    13.03.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in the node/pool: implements the JSON-RPC 2.0 (and legacy 1.0) wire
// protocol for the client side. It serializes outgoing method/notification
// requests into JSON and parses/validates incoming responses, converting
// server-reported errors into JsonRpcExceptions. This sits above the transport
// (IClientConnector) and is what the node/pool uses to speak RPC to digibyted.

#include "rpcprotocolclient.h"
#include <jsonrpccpp/common/jsonparser.h>

using namespace jsonrpc;

const std::string RpcProtocolClient::KEY_PROTOCOL_VERSION = "jsonrpc";
const std::string RpcProtocolClient::KEY_PROCEDURE_NAME = "method";
const std::string RpcProtocolClient::KEY_ID = "id";
const std::string RpcProtocolClient::KEY_PARAMETER = "params";
const std::string RpcProtocolClient::KEY_AUTH = "auth";
const std::string RpcProtocolClient::KEY_RESULT = "result";
const std::string RpcProtocolClient::KEY_ERROR = "error";
const std::string RpcProtocolClient::KEY_ERROR_CODE = "code";
const std::string RpcProtocolClient::KEY_ERROR_MESSAGE = "message";
const std::string RpcProtocolClient::KEY_ERROR_DATA = "data";

RpcProtocolClient::RpcProtocolClient(clientVersion_t version, bool omitEndingLineFeed) : version(version), omitEndingLineFeed(omitEndingLineFeed) {}

/**
 * @brief Serializes a JSON-RPC request into its string form.
 *
 * Builds the request object (id fixed to 1 here) via the private BuildRequest
 * overload, then writes it compactly (no indentation) into @p result.
 *
 * @param method         Method or notification name to invoke.
 * @param parameter      Parameters as a JSON value (omitted when null).
 * @param result         Receives the serialized JSON request string.
 * @param isNotification When true, no response id is expected (notification).
 */
void RpcProtocolClient::BuildRequest(const std::string &method, const Json::Value &parameter, std::string &result, bool isNotification) {
  Json::Value request;
  Json::StreamWriterBuilder wbuilder;
  wbuilder["indentation"] = "";
  this->BuildRequest(1, method, parameter, request, isNotification);

  result = Json::writeString(wbuilder, request);
}

/**
 * @brief Parses a raw response string and extracts the RPC result.
 *
 * Parses @p response as JSON and delegates to the Json::Value overload. A parse
 * failure (malformed JSON or a Json library exception) is reported as a
 * JsonRpcException with ERROR_RPC_JSON_PARSE_ERROR.
 *
 * @param response Raw response text received from the server.
 * @param result   Receives the extracted "result" value on success.
 * @throw JsonRpcException on parse errors or an error object in the response.
 */
void RpcProtocolClient::HandleResponse(const std::string &response, Json::Value &result) {
  Json::Value value;

  try {
    if (std::istringstream(response) >> value) {
      this->HandleResponse(value, result);
    } else {
      throw JsonRpcException(Errors::ERROR_RPC_JSON_PARSE_ERROR, " " + response);
    }
  } catch (Json::Exception &e) {
    throw JsonRpcException(Errors::ERROR_RPC_JSON_PARSE_ERROR, " " + response);
  }
}

/**
 * @brief Validates a parsed response and pulls out its result or error.
 *
 * Checks structural validity for the configured protocol version; if valid and
 * carrying an error, throws via throwErrorException(), otherwise copies the
 * "result" member into @p result. An invalid envelope raises
 * ERROR_CLIENT_INVALID_RESPONSE.
 *
 * @param value  Parsed JSON response object.
 * @param result Receives the response "result" value on success.
 * @return The response "id" value.
 * @throw JsonRpcException on an invalid response or a server-reported error.
 */
Json::Value RpcProtocolClient::HandleResponse(const Json::Value &value, Json::Value &result) {
  if (this->ValidateResponse(value)) {
    if (this->HasError(value)) {
      this->throwErrorException(value);
    } else {
      result = value[KEY_RESULT];
    }
  } else {
    throw JsonRpcException(Errors::ERROR_CLIENT_INVALID_RESPONSE, " " + value.toStyledString());
  }
  return value[KEY_ID];
}

/**
 * @brief Populates a JSON-RPC request object (private core builder).
 *
 * Sets "jsonrpc":"2.0" for V2, the method name, and parameters when non-null.
 * For a normal call the numeric @p id is written; for a V1 notification the id
 * is written as null, and for a V2 notification no id is written at all.
 *
 * @param id             Request id to embed (ignored for notifications except V1).
 * @param method         Method or notification name.
 * @param parameter      Parameters JSON value (added only when not null).
 * @param result         Request object being populated.
 * @param isNotification When true, builds a notification rather than a call.
 */
void RpcProtocolClient::BuildRequest(int id, const std::string &method, const Json::Value &parameter, Json::Value &result, bool isNotification) {
  if (this->version == JSONRPC_CLIENT_V2)
    result[KEY_PROTOCOL_VERSION] = "2.0";
  result[KEY_PROCEDURE_NAME] = method;
  if (parameter != Json::nullValue)
    result[KEY_PARAMETER] = parameter;
  if (!isNotification)
    result[KEY_ID] = id;
  else if (this->version == JSONRPC_CLIENT_V1)
    result[KEY_ID] = Json::nullValue;
}

/**
 * @brief Converts the "error" member of a response into a thrown exception.
 *
 * Always throws. Includes the error message and, when present, the "data"
 * payload; if no string message is present, throws with just the error code.
 *
 * @param response A response object known to contain an "error" member.
 * @throw JsonRpcException carrying the server's error code/message/data.
 */
void RpcProtocolClient::throwErrorException(const Json::Value &response) {
  if (response[KEY_ERROR].isMember(KEY_ERROR_MESSAGE) && response[KEY_ERROR][KEY_ERROR_MESSAGE].isString()) {
    if (response[KEY_ERROR].isMember(KEY_ERROR_DATA)) {
      throw JsonRpcException(response[KEY_ERROR][KEY_ERROR_CODE].asInt(), response[KEY_ERROR][KEY_ERROR_MESSAGE].asString(),
                             response[KEY_ERROR][KEY_ERROR_DATA]);
    } else {
      throw JsonRpcException(response[KEY_ERROR][KEY_ERROR_CODE].asInt(), response[KEY_ERROR][KEY_ERROR_MESSAGE].asString());
    }
  } else {
    throw JsonRpcException(response[KEY_ERROR][KEY_ERROR_CODE].asInt());
  }
}

/**
 * @brief Checks that a response is a structurally valid JSON-RPC envelope.
 *
 * Requires an object with an "id". For V1: exactly one of result/error present
 * and non-null, with a well-formed error object if any. For V2: "jsonrpc"=="2.0"
 * and exactly one of result/error present, with a well-formed error object.
 *
 * @param response Parsed response to validate.
 * @return true if the envelope conforms to the configured protocol version.
 */
bool RpcProtocolClient::ValidateResponse(const Json::Value &response) {
  if (!response.isObject() || !response.isMember(KEY_ID))
    return false;

  if (this->version == JSONRPC_CLIENT_V1) {
    if (!response.isMember(KEY_RESULT) || !response.isMember(KEY_ERROR))
      return false;
    if (!response[KEY_RESULT].isNull() && !response[KEY_ERROR].isNull())
      return false;
    if (!response[KEY_ERROR].isNull() &&
        !(response[KEY_ERROR].isObject() && response[KEY_ERROR].isMember(KEY_ERROR_CODE) && response[KEY_ERROR][KEY_ERROR_CODE].isIntegral()))
      return false;
  } else if (this->version == JSONRPC_CLIENT_V2) {
    if (!response.isMember(KEY_PROTOCOL_VERSION) || response[KEY_PROTOCOL_VERSION] != "2.0")
      return false;
    if (response.isMember(KEY_RESULT) && response.isMember(KEY_ERROR))
      return false;
    if (!response.isMember(KEY_RESULT) && !response.isMember(KEY_ERROR))
      return false;
    if (response.isMember(KEY_ERROR) &&
        !(response[KEY_ERROR].isObject() && response[KEY_ERROR].isMember(KEY_ERROR_CODE) && response[KEY_ERROR][KEY_ERROR_CODE].isIntegral()))
      return false;
  }

  return true;
}

/**
 * @brief Reports whether a validated response carries an error.
 *
 * V1: true when the "error" member is non-null. V2: true when an "error" member
 * is present at all.
 *
 * @param response A response already passed through ValidateResponse().
 * @return true if the response represents an RPC error.
 */
bool RpcProtocolClient::HasError(const Json::Value &response) {
  if (this->version == JSONRPC_CLIENT_V1 && !response[KEY_ERROR].isNull())
    return true;
  else if (this->version == JSONRPC_CLIENT_V2 && response.isMember(KEY_ERROR))
    return true;
  return false;
}
