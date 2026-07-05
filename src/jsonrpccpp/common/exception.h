/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    exception.h
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Role in DigiAsset for Windows:
 * Part of the bundled libjson-rpc-cpp library. Declares the exception type
 * thrown throughout the JSON-RPC layer (used when communicating with the
 * DigiByte Core node) to signal protocol, transport, and application errors.
 */

#ifndef JSONRPC_CPP_EXCEPTION_H_
#define JSONRPC_CPP_EXCEPTION_H_

#include <exception>
#include <sstream>
#include <string>

#include "errors.h"

namespace jsonrpc {
  /**
   * Exception carrying a JSON-RPC error code, message, and optional data value.
   * Constructed either from a known error code (message auto-looked-up via
   * Errors::GetErrorMessage) or from a free-form message. Inherits std::exception
   * so what() returns a formatted description of code, message, and data.
   */
  class JsonRpcException : public std::exception {
  public:
    // Build from a known error code; message is looked up from the Errors table.
    JsonRpcException(int code);
    // As above, but appends a caller-supplied detail message after the code text.
    JsonRpcException(int code, const std::string &message);
    // As above, plus an arbitrary JSON data payload attached to the error.
    JsonRpcException(int code, const std::string &message, const Json::Value &data);
    // Build a code-less exception carrying only a free-form message (code == 0).
    JsonRpcException(const std::string &message);

    virtual ~JsonRpcException() throw();

    int GetCode() const;
    const std::string &GetMessage() const;
    const Json::Value &GetData() const;

    virtual const char *what() const throw();

  private:
    int code;
    std::string message;
    std::string whatString;
    Json::Value data;
    // Composes whatString from code/message/data; called by each constructor.
    void setWhatMessage();
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_EXCEPTION_H_ */
