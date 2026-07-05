/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    batchresponse.cpp
 * @date    10/9/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Implementation of BatchResponse: stores the parsed per-id results (and error
// records) returned for a JSON-RPC batch request and exposes lookup by request
// id, so callers can retrieve each Core RPC call's result or error.

#include "batchresponse.h"
#include <algorithm>

using namespace jsonrpc;
using namespace std;

BatchResponse::BatchResponse() {}

// Record one response keyed by its request id. If isError, the id is also
// tracked in the error list; the response value (result or error object) is
// stored either way. Used internally while parsing the batch reply.

void BatchResponse::addResponse(Json::Value &id, Json::Value response, bool isError) {
  if (isError) {
    errorResponses.push_back(id);
  }
  responses[id] = response;
}

// Return the result value for an integer request id, or null if that id
// produced an error. Convenience wrapper over the Json::Value overload.
Json::Value BatchResponse::getResult(int id) {
  Json::Value result;
  Json::Value i = id;
  getResult(i, result);
  return result;
}

// Write the stored result for `id` into `result`, or Json::nullValue if that
// id carries an error (checked via getErrorCode).
void BatchResponse::getResult(Json::Value &id, Json::Value &result) {
  if (getErrorCode(id) == 0)
    result = responses[id];
  else
    result = Json::nullValue;
}

// Return the JSON-RPC error "code" for `id` if it is in the error list,
// otherwise 0 (no error).
int BatchResponse::getErrorCode(Json::Value &id) {
  if (std::find(errorResponses.begin(), errorResponses.end(), id) != errorResponses.end()) {
    return responses[id]["code"].asInt();
  }
  return 0;
}

// Return the JSON-RPC error "message" for `id` if it errored, else "".
string BatchResponse::getErrorMessage(Json::Value &id) {
  if (std::find(errorResponses.begin(), errorResponses.end(), id) != errorResponses.end()) {
    return responses[id]["message"].asString();
  }
  return "";
}

// Integer-id convenience overload of getErrorMessage.
string BatchResponse::getErrorMessage(int id) {
  Json::Value i = id;
  return getErrorMessage(i);
}

// True if any call in the batch returned an error.
bool BatchResponse::hasErrors() { return !errorResponses.empty(); }
