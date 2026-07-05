/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    batchcall.h
 * @date    15.10.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

#ifndef JSONRPC_CPP_BATCHCALL_H
#define JSONRPC_CPP_BATCHCALL_H

#include <jsonrpccpp/common/jsonparser.h>

namespace jsonrpc {
  /**
   * @brief Builder for a JSON-RPC 2.0 batch request.
   *
   * Collects multiple method calls (and notifications) into one JSON array and
   * serializes it, letting the node/pool bundle many DigiByte Core RPC calls
   * into a single HTTP request. Returned call ids are used to match entries in
   * the corresponding BatchResponse.
   */
  class BatchCall {
  public:
    BatchCall();

    /**
     * @brief addCall
     * @param methodname
     * @param params
     * @param isNotification
     * @return the id of the geneared request inside the batchcall
     */
    int addCall(const std::string &methodname, const Json::Value &params, bool isNotification = false);
    std::string toString(bool fast = true) const;

  private:
    Json::Value result;
    int id;
  };
} // namespace jsonrpc

#endif // JSONRPC_CPP_BATCHCALL_H
