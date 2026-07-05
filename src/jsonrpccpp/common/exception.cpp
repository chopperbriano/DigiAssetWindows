/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    exception.cpp
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Role in DigiAsset for Windows:
 * Part of the bundled libjson-rpc-cpp library. Implements JsonRpcException:
 * how error codes become messages, how caller detail and data are merged, and
 * how the final what() text is composed for the JSON-RPC/node communication layer.
 */

#include "exception.h"

using namespace jsonrpc;

// Code-only ctor: message is the standard text for that code.
JsonRpcException::JsonRpcException(int code) : code(code), message(Errors::GetErrorMessage(code)) { this->setWhatMessage(); }

// Code + detail ctor: prefixes the standard code text (with ": ") ahead of the caller's message.
JsonRpcException::JsonRpcException(int code, const std::string &message) : code(code), message(Errors::GetErrorMessage(code)) {
  if (!this->message.empty())
    this->message = this->message + ": ";
  this->message = this->message + message;
  this->setWhatMessage();
}

// Code + detail + data ctor: as above, but also carries a JSON data payload.
JsonRpcException::JsonRpcException(int code, const std::string &message, const Json::Value &data)
    : code(code), message(Errors::GetErrorMessage(code)), data(data) {
  if (!this->message.empty())
    this->message = this->message + ": ";
  this->message = this->message + message;
  this->setWhatMessage();
}

// Message-only ctor: no numeric code (code == 0), used for generic/library errors.
JsonRpcException::JsonRpcException(const std::string &message) : code(0), message(message) { this->setWhatMessage(); }

JsonRpcException::~JsonRpcException() throw() {}

int JsonRpcException::GetCode() const { return code; }

const std::string &JsonRpcException::GetMessage() const { return message; }

const Json::Value &JsonRpcException::GetData() const { return data; }

const char *JsonRpcException::what() const throw() { return this->whatString.c_str(); }

// Builds the what() string: "Exception <code> : <message>" (plus ", data: ..."
// when a data payload is present), or just the message when code == 0.
void JsonRpcException::setWhatMessage() {
  if (this->code != 0) {
    std::stringstream ss;
    ss << "Exception " << this->code << " : " << this->message;
    if (data != Json::nullValue)
      ss << ", data: " << data.toStyledString();
    this->whatString = ss.str();
  } else {
    this->whatString = this->message;
  }
}
