/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    httpclient.cpp
 * @date    02.01.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// File role: Implements HttpClient, the libcurl-backed HTTP transport for
// JSON-RPC. Used by the node to POST requests to the local DigiByte Core RPC
// endpoint. Also sets up a process-wide curl global init/cleanup guard and a
// small growable string buffer used as libcurl's write target.

#include "httpclient.h"
#include <cstdlib>
#include <curl/curl.h>
#include <string.h>
#include <string>

#include <iostream>

using namespace jsonrpc;

// RAII guard that performs libcurl's one-time global init at startup and
// global cleanup at shutdown (see the linked curl docs below).
class curl_initializer {
public:
  curl_initializer() { curl_global_init(CURL_GLOBAL_ALL); }
  ~curl_initializer() { curl_global_cleanup(); }
};

// See here: http://curl.haxx.se/libcurl/c/curl_global_init.html
static curl_initializer _curl_init = curl_initializer();

/**
 * taken from
 * http://stackoverflow.com/questions/2329571/c-libcurl-get-output-into-a-string
 */
// Growable heap buffer used to accumulate the HTTP response body.
struct string {
  char *ptr;
  size_t len;
};

// libcurl CURLOPT_WRITEFUNCTION callback: appends the received chunk to `s`,
// reallocating as needed and keeping the buffer NUL-terminated. Returns the
// number of bytes consumed (must equal size*nmemb for curl to proceed).
static size_t writefunc(void *ptr, size_t size, size_t nmemb, struct string *s) {
  size_t new_len = s->len + size * nmemb;
  s->ptr = (char *)realloc(s->ptr, new_len + 1);
  memcpy(s->ptr + s->len, ptr, size * nmemb);
  s->ptr[new_len] = '\0';
  s->len = new_len;
  return size * nmemb;
}

// Initializes an empty, NUL-terminated string buffer for writefunc to grow.
void init_string(struct string *s) {
  s->len = 0;
  s->ptr = static_cast<char *>(malloc(s->len + 1));
  s->ptr[0] = '\0';
}

// Stores the target URL, defaults the timeout to 10000 ms, and creates the
// reusable CURL easy handle.
HttpClient::HttpClient(const std::string &url) : url(url) {
  this->timeout = 10000;
  curl = curl_easy_init();
}

HttpClient::~HttpClient() { curl_easy_cleanup(curl); }

// Configures the CURL handle (URL, write callback/buffer, POST body, headers,
// timeout), performs the request, and copies the response body into `result`.
// Frees the header list and response buffer, then throws JsonRpcException on a
// libcurl error (7=connect failed, 28=timeout are annotated) or on any non-2xx
// HTTP response code.
void HttpClient::SendRPCMessage(const std::string &message, std::string &result) {

  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt(curl, CURLOPT_URL, this->url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);

  CURLcode res;

  struct string s;
  init_string(&s);

  struct curl_slist *headers = NULL;

  for (std::map<std::string, std::string>::iterator header = this->headers.begin(); header != this->headers.end(); ++header) {
    headers = curl_slist_append(headers, (header->first + ": " + header->second).c_str());
  }

  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "charsets: utf-8");

  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, message.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);

  res = curl_easy_perform(curl);

  result = s.ptr;
  free(s.ptr);
  curl_slist_free_all(headers);
  if (res != CURLE_OK) {
    std::stringstream str;
    str << "libcurl error: " << res;

    if (res == 7)
      str << " -> Could not connect to " << this->url;
    else if (res == 28)
      str << " -> Operation timed out";
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, str.str());
  }

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  if (http_code / 100 != 2) {
    throw JsonRpcException(Errors::ERROR_RPC_INTERNAL_ERROR, result);
  }
}

void HttpClient::SetUrl(const std::string &url) { this->url = url; }

void HttpClient::SetTimeout(long timeout) { this->timeout = timeout; }

void HttpClient::AddHeader(const std::string &attr, const std::string &val) { this->headers[attr] = val; }

void HttpClient::RemoveHeader(const std::string &attr) { this->headers.erase(attr); }
