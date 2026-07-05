/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    httpserver.cpp
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 *
 * Implementation of HttpServer, a libmicrohttpd-backed JSON-RPC server
 * connector. This is the primary transport by which the DigiAsset node and the
 * DigiAsset pool server expose their JSON-RPC HTTP API: it accepts POST bodies
 * as JSON-RPC requests, routes them to per-URL handlers, answers CORS
 * pre-flight OPTIONS requests, and can optionally run over TLS or bound to
 * loopback only.
 ************************************************************************/

#include "httpserver.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <jsonrpccpp/common/specificationparser.h>
#include <sstream>

using namespace jsonrpc;
using namespace std;

#define BUFFERSIZE 65536

// Per-connection state attached to each libmicrohttpd request via con_cls:
// accumulates the uploaded request body and carries the owning server plus the
// HTTP status code to return.
struct mhd_coninfo {
  struct MHD_PostProcessor *postprocessor;
  MHD_Connection *connection;
  stringstream request;
  HttpServer *server;
  int code;
};

// Construct the server bound to `port` with a `threads`-sized worker pool. If
// both sslcert and sslkey paths are given the daemon runs over TLS. The daemon
// is not started until StartListening() is called.
HttpServer::HttpServer(int port, const std::string &sslcert, const std::string &sslkey, int threads)
    : AbstractServerConnector(), port(port), threads(threads), running(false), path_sslcert(sslcert), path_sslkey(sslkey), daemon(NULL), bindlocalhost(false) {}

HttpServer::~HttpServer() {}

// Resolve the connection handler for a request URL. A global handler set via
// SetHandler() takes precedence; otherwise the per-URL handler registered with
// SetUrlHandler() is returned, or NULL if none matches.
IClientConnectionHandler *HttpServer::GetHandler(const std::string &url) {
  if (AbstractServerConnector::GetHandler() != NULL)
    return AbstractServerConnector::GetHandler();
  map<string, IClientConnectionHandler *>::iterator it = this->urlhandler.find(url);
  if (it != this->urlhandler.end())
    return it->second;
  return NULL;
}

HttpServer &HttpServer::BindLocalhost() {
  this->bindlocalhost = true;
  return *this;
}

// Start the embedded microhttpd daemon (idempotent while already running).
// Selects the best available event backend (epoll/poll/select), then starts in
// one of three modes: loopback-bound plaintext (BindLocalhost), TLS when both
// cert and key paths are set, or plain HTTP otherwise. All requests are routed
// to HttpServer::callback with a worker thread pool of size `threads`.
// @return true if the daemon started (running), false on failure.
bool HttpServer::StartListening() {
  if (!this->running) {
    const bool has_epoll = (MHD_is_feature_supported(MHD_FEATURE_EPOLL) == MHD_YES);
    const bool has_poll = (MHD_is_feature_supported(MHD_FEATURE_POLL) == MHD_YES);
    unsigned int mhd_flags = MHD_USE_DUAL_STACK;

    if (has_epoll)
// In MHD version 0.9.44 the flag is renamed to
// MHD_USE_EPOLL_INTERNALLY_LINUX_ONLY. In later versions both
// are deprecated.
#if defined(MHD_USE_EPOLL_INTERNALLY)
      mhd_flags = MHD_USE_EPOLL_INTERNALLY;
#else
      mhd_flags = MHD_USE_EPOLL_INTERNALLY_LINUX_ONLY;
#endif
    else if (has_poll)
      mhd_flags = MHD_USE_POLL_INTERNALLY;

    if (this->bindlocalhost) {
      memset(&this->loopback_addr, 0, sizeof(this->loopback_addr));
      loopback_addr.sin_family = AF_INET;
      loopback_addr.sin_port = htons(this->port);
      loopback_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      this->daemon = MHD_start_daemon(mhd_flags, this->port, NULL, NULL, HttpServer::callback, this, MHD_OPTION_THREAD_POOL_SIZE, this->threads,
                                      MHD_OPTION_SOCK_ADDR, (struct sockaddr *)(&(this->loopback_addr)), MHD_OPTION_END);

    } else if (!this->path_sslcert.empty() && !this->path_sslkey.empty()) {
      try {
        SpecificationParser::GetFileContent(this->path_sslcert, this->sslcert);
        SpecificationParser::GetFileContent(this->path_sslkey, this->sslkey);

        this->daemon =
            MHD_start_daemon(MHD_USE_SSL | mhd_flags, this->port, NULL, NULL, HttpServer::callback, this, MHD_OPTION_HTTPS_MEM_KEY, this->sslkey.c_str(),
                             MHD_OPTION_HTTPS_MEM_CERT, this->sslcert.c_str(), MHD_OPTION_THREAD_POOL_SIZE, this->threads, MHD_OPTION_END);
      } catch (JsonRpcException &ex) {
        return false;
      }
    } else {
      this->daemon =
          MHD_start_daemon(mhd_flags, this->port, NULL, NULL, HttpServer::callback, this, MHD_OPTION_THREAD_POOL_SIZE, this->threads, MHD_OPTION_END);
    }
    if (this->daemon != NULL)
      this->running = true;
  }
  return this->running;
}

// Stop the microhttpd daemon if running and clear the running flag.
// @return always true.
bool HttpServer::StopListening() {
  if (this->running) {
    MHD_stop_daemon(this->daemon);
    this->running = false;
  }
  return true;
}

// Queue an HTTP response body for the connection identified by addInfo (an
// mhd_coninfo*). Sets Content-Type: application/json and permissive CORS,
// using the status code stored on the connection.
// @return true if microhttpd accepted the response.
bool HttpServer::SendResponse(const string &response, void *addInfo) {
  struct mhd_coninfo *client_connection = static_cast<struct mhd_coninfo *>(addInfo);
  struct MHD_Response *result = MHD_create_response_from_buffer(response.size(), (void *)response.c_str(), MHD_RESPMEM_MUST_COPY);

  MHD_add_response_header(result, "Content-Type", "application/json");
  MHD_add_response_header(result, "Access-Control-Allow-Origin", "*");

  int ret = MHD_queue_response(client_connection->connection, client_connection->code, result);
  MHD_destroy_response(result);
  return ret == MHD_YES;
}

// Queue the empty-body reply to a CORS pre-flight OPTIONS request, advertising
// the allowed methods/headers (POST, OPTIONS) and permissive CORS.
// @return true if microhttpd accepted the response.
bool HttpServer::SendOptionsResponse(void *addInfo) {
  struct mhd_coninfo *client_connection = static_cast<struct mhd_coninfo *>(addInfo);
  struct MHD_Response *result = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_MUST_COPY);

  MHD_add_response_header(result, "Allow", "POST, OPTIONS");
  MHD_add_response_header(result, "Access-Control-Allow-Origin", "*");
  MHD_add_response_header(result, "Access-Control-Allow-Headers", "origin, content-type, accept");
  MHD_add_response_header(result, "DAV", "1");

  int ret = MHD_queue_response(client_connection->connection, client_connection->code, result);
  MHD_destroy_response(result);
  return ret == MHD_YES;
}

// Register a handler for requests to a specific URL path and clear any global
// handler so per-URL routing takes effect.
void HttpServer::SetUrlHandler(const string &url, IClientConnectionHandler *handler) {
  this->urlhandler[url] = handler;
  this->SetHandler(NULL);
}

// libmicrohttpd access-handler callback, invoked (possibly repeatedly) per
// request. On the first call it allocates the per-connection mhd_coninfo. For
// POST it accumulates the upload body across calls, then on the final call
// routes it to the matching handler and sends the JSON-RPC response (or a 500
// if no handler is registered). OPTIONS yields the CORS pre-flight reply; any
// other method yields 405. The connection state is freed after the reply.
// @param cls the HttpServer* passed at daemon start.
// @return MHD_YES to keep processing, MHD_NO on failure.
HttpServer::MicroHttpdResult HttpServer::callback(void *cls, MHD_Connection *connection, const char *url, const char *method, const char *version,
                                                  const char *upload_data, size_t *upload_data_size, void **con_cls) {
  (void)version;
  if (*con_cls == NULL) {
    struct mhd_coninfo *client_connection = new mhd_coninfo;
    client_connection->connection = connection;
    client_connection->server = static_cast<HttpServer *>(cls);
    *con_cls = client_connection;
    return MHD_YES;
  }
  struct mhd_coninfo *client_connection = static_cast<struct mhd_coninfo *>(*con_cls);

  if (string("POST") == method) {
    if (*upload_data_size != 0) {
      client_connection->request.write(upload_data, *upload_data_size);
      *upload_data_size = 0;
      return MHD_YES;
    } else {
      string response;
      IClientConnectionHandler *handler = client_connection->server->GetHandler(string(url));
      if (handler == NULL) {
        client_connection->code = MHD_HTTP_INTERNAL_SERVER_ERROR;
        client_connection->server->SendResponse("No client connection handler found", client_connection);
      } else {
        client_connection->code = MHD_HTTP_OK;
        handler->HandleRequest(client_connection->request.str(), response);
        client_connection->server->SendResponse(response, client_connection);
      }
    }
  } else if (string("OPTIONS") == method) {
    client_connection->code = MHD_HTTP_OK;
    client_connection->server->SendOptionsResponse(client_connection);
  } else {
    client_connection->code = MHD_HTTP_METHOD_NOT_ALLOWED;
    client_connection->server->SendResponse("Not allowed HTTP Method", client_connection);
  }

  if (client_connection != nullptr) {
    delete client_connection;
  }
  *con_cls = NULL;

  return MHD_YES;
}
