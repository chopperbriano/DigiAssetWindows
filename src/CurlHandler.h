//
// Created by mctrivia on 01/02/24.
//

// CurlHandler.h
// Thin libcurl wrapper used throughout the node/pool for simple blocking HTTP.
// Provides free functions for GET/POST (returning the body as a string or
// streaming it to a file) plus a raw JSON POST. Used e.g. to talk to the local
// IPFS/kubo HTTP API, fetch downloads (kubo binaries), and by pool components
// making HTTP requests. Each thread reuses one CURL easy handle for connection
// reuse (see CurlHandler.cpp). Not a class - just a namespace of helpers.

#ifndef DIGIASSET_CORE_CURLHANDLER_H
#define DIGIASSET_CORE_CURLHANDLER_H



#include <curl/curl.h>
#include <map>
#include <mutex>
#include <string>

namespace CurlHandler {

    // Performs a blocking HTTP GET and returns the full response body.
    // timeout is in milliseconds (0 = no timeout). Throws exceptionTimeout on
    // timeout, std::runtime_error on other transport errors.
    std::string get(const std::string& url, unsigned int timeout = 0);
    // Performs a blocking application/x-www-form-urlencoded HTTP POST built from
    // the key/value pairs in data, returning the full response body.
    // timeout is in milliseconds (0 = no timeout). Throws exceptionTimeout on
    // timeout, std::runtime_error on other transport errors.
    std::string post(const std::string& url, const std::map<std::string, std::string>& data = {}, unsigned int timeout = 0);
    // Raw JSON POST. Returns the HTTP status code via statusCode, body via responseBody.
    // Throws on transport/network errors only; non-2xx is NOT an exception.
    long postJson(const std::string& url, const std::string& body, std::string& responseBody, unsigned int timeout = 0);
    // Performs a blocking HTTP GET and writes the response body directly to
    // fileName (opened "wb"). timeout in ms (0 = none). Throws exceptionTimeout
    // on timeout, std::runtime_error on file-open or other transport errors.
    void getDownload(const std::string& url, const std::string& fileName, unsigned int timeout = 0);
    // Performs a blocking form-urlencoded HTTP POST and writes the response body
    // directly to fileName (opened "wb"). timeout in ms (0 = none). Throws
    // exceptionTimeout on timeout, std::runtime_error on other errors.
    void postDownload(const std::string& url, const std::string& fileName, const std::map<std::string, std::string>& data = {}, unsigned int timeout = 0);

    // Thrown by the above functions when a request exceeds its timeout.
    class exceptionTimeout : public std::exception {
    public:
        char* what() {
            return const_cast<char*>("request timed out");
        }
    };

}; // namespace CurlHandler

#endif //DIGIASSET_CORE_CURLHANDLER_H
