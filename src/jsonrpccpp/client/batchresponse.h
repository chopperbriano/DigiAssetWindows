/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    batchresponse.h
 * @date    10/9/2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

#ifndef JSONRPC_BATCHRESPONSE_H
#define JSONRPC_BATCHRESPONSE_H

#include <jsonrpccpp/common/jsonparser.h>
#include <map>

namespace jsonrpc {

  /**
   * @brief The BatchResponse class provides a simple interface for handling batch responses.
   *
   * Holds the parsed replies to a JSON-RPC batch (see BatchCall), indexed by the
   * request id returned from BatchCall::addCall, and lets the node/pool fetch
   * each individual result or its error code/message.
   */
  class BatchResponse {
  public:
    BatchResponse();

    /**
     * @brief addResponse method is used only internally by the framework
     * @param id
     * @param response
     * @param isError
     */
    void addResponse(Json::Value &id, Json::Value response, bool isError = false);

    /**
     * @brief getResult method gets the result for a given request id (returned by BatchCall::addCall.
     * You should always invoke getErrorCode() first to check if the result is valid.
     * @param id
     * @return
     */
    Json::Value getResult(int id);

    /**
     * @brief getResult overload writing into `result`; yields null on error.
     */
    void getResult(Json::Value &id, Json::Value &result);

    /**
     * @brief getErrorCode method checks if for a given id, an error occurred in the batch request.
     * @param id
     */
    int getErrorCode(Json::Value &id);

    /**
     * @brief getErrorMessage method gets the corresponding error message.
     * @param id
     * @return the error message in case of an error, an empty string if no error was found for the provided id.
     */
    std::string getErrorMessage(Json::Value &id);

    /**
     * @brief integer-id convenience overload of getErrorMessage.
     */
    std::string getErrorMessage(int id);

    /**
     * @brief hasErrors returns true if any call in the batch failed.
     */
    bool hasErrors();

  private:
    std::map<Json::Value, Json::Value> responses;
    std::vector<Json::Value> errorResponses;
  };

} // namespace jsonrpc

#endif // JSONRPC_BATCHRESPONSE_H
