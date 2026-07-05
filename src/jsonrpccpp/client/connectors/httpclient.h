/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    httpclient.h
 * @date    02.01.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// File role: Declares HttpClient, the libcurl-backed IClientConnector that
// POSTs JSON-RPC requests over HTTP. This is the transport the node uses to
// reach the local DigiByte Core JSON-RPC endpoint; it holds the target URL,
// a timeout, extra HTTP headers, and a reusable CURL handle.

#ifndef JSONRPC_CPP_HTTPCLIENT_H_
#define JSONRPC_CPP_HTTPCLIENT_H_

#include "../iclientconnector.h"
#include <curl/curl.h>
#include <jsonrpccpp/common/exception.h>
#include <map>

namespace jsonrpc {
  // HTTP(S) connector: sends each JSON-RPC message as an HTTP POST via libcurl
  // and returns the response body.
  class HttpClient : public IClientConnector {
  public:
    // Creates a client targeting `url` with a default 10s timeout and an
    // initialized CURL easy handle.
    HttpClient(const std::string &url);
    virtual ~HttpClient();
    // POSTs `message` to the configured URL with the accumulated headers plus
    // Content-Type: application/json, storing the body in `result`. Throws
    // JsonRpcException on a libcurl error (e.g. connect/timeout) or on any
    // non-2xx HTTP status.
    virtual void SendRPCMessage(const std::string &message, std::string &result);

    // Changes the target endpoint URL.
    void SetUrl(const std::string &url);
    // Sets the per-request timeout in milliseconds.
    void SetTimeout(long timeout);

    // Adds/overwrites an extra HTTP header sent with every request.
    void AddHeader(const std::string &attr, const std::string &val);
    // Removes a previously added extra header.
    void RemoveHeader(const std::string &attr);

  protected:
    std::map<std::string, std::string> headers;
    std::string url;

    /**
     * @brief timeout for http request in milliseconds
     */
    long timeout;
    CURL *curl;
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_HTTPCLIENT_H_ */
