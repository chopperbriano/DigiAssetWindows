/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    errors.h
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Role in DigiAsset for Windows:
 * Part of the bundled libjson-rpc-cpp library that powers the JSON-RPC layer
 * used to talk to the DigiByte Core full node (and to expose RPC endpoints).
 * This header defines the canonical table of JSON-RPC error codes and a helper
 * to map a numeric code to a human-readable message. The codes cover the
 * official JSON-RPC 2.0 spec errors plus library-specific server/client errors.
 */

#ifndef JSONRPC_CPP_ERRORS_H_
#define JSONRPC_CPP_ERRORS_H_

#include <map>
#include <string>

#include "jsonparser.h"

namespace jsonrpc {
  class JsonRpcException;

  /**
   * Central registry of JSON-RPC error codes and their messages.
   * Exposes the standard and library-specific error-code constants and a
   * lookup from code to text. The nested _init object/_initializer static
   * populates the internal code-to-message map once at static-init time.
   */
  class Errors {
  public:
    /**
     * @return error message to corresponding error code.
     */
    static std::string GetErrorMessage(int errorCode);

    /**
     * Static initializer helper: its constructor fills possibleErrors with the
     * code-to-message pairs. A single static instance (_initializer) runs it
     * automatically before main().
     */
    static class _init {
    public:
      _init();
    } _initializer;

    /**
     * Official JSON-RPC 2.0 Errors
     */
    static const int ERROR_RPC_JSON_PARSE_ERROR;
    static const int ERROR_RPC_METHOD_NOT_FOUND;
    static const int ERROR_RPC_INVALID_REQUEST;
    static const int ERROR_RPC_INVALID_PARAMS;
    static const int ERROR_RPC_INTERNAL_ERROR;

    /**
     * Server Library Errors
     */
    static const int ERROR_SERVER_PROCEDURE_IS_METHOD;
    static const int ERROR_SERVER_PROCEDURE_IS_NOTIFICATION;
    static const int ERROR_SERVER_PROCEDURE_POINTER_IS_NULL;
    static const int ERROR_SERVER_PROCEDURE_SPECIFICATION_NOT_FOUND;
    static const int ERROR_SERVER_PROCEDURE_SPECIFICATION_SYNTAX;
    static const int ERROR_SERVER_CONNECTOR;

    /**
     * Client Library Errors
     */
    static const int ERROR_CLIENT_CONNECTOR;
    static const int ERROR_CLIENT_INVALID_RESPONSE;

  private:
    static std::map<int, std::string> possibleErrors;
  };
} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_ERRORS_H_ */
