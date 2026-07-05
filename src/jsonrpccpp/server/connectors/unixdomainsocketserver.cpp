/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    unixdomainsocketserver.cpp
 * @date    07.05.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Role in DigiAsset for Windows:
 * Implementation of the Unix-domain-socket JSON-RPC server connector from the
 * bundled libjson-rpc-cpp dependency. It listens on a filesystem socket path
 * and serves each incoming connection by reading a delimiter-terminated
 * request, dispatching it through the RPC handler, and writing back the
 * response. This POSIX-only connector is not used on the Windows build (which
 * uses WindowsTcpSocketServer); it is kept for cross-platform parity.
 */

#include "unixdomainsocketserver.h"
#include "../../common/sharedconstants.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>

using namespace jsonrpc;
using namespace std;

UnixDomainSocketServer::UnixDomainSocketServer(const string &socket_path, size_t threads)
    : AbstractThreadedServer(threads), socket_path(socket_path), socket_fd(-1) {}

/* Destructor: closes the listening socket if open and removes the socket file
 * from the filesystem so the path can be reused on the next start. */
UnixDomainSocketServer::~UnixDomainSocketServer() {
  if (this->socket_fd != -1)
    close(this->socket_fd);
  unlink(this->socket_path.c_str());
}

/**
 * @brief Creates, binds and listens on the Unix-domain socket.
 *
 * Fails (returns false) if the socket path already exists, or if socket
 * creation, bind or listen fails. Puts the listening socket into non-blocking
 * mode so the base AbstractThreadedServer can poll for connections.
 * @return true if the listener is ready, false on any error.
 */
bool UnixDomainSocketServer::InitializeListener() {
  if (access(this->socket_path.c_str(), F_OK) != -1)
    return false;

  this->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (this->socket_fd < 0) {
    return false;
  }

  // Set to non-blocking mode
  fcntl(this->socket_fd, F_SETFL, FNDELAY);

  memset(&(this->address), 0, sizeof(struct sockaddr_un));
  this->address.sun_family = AF_UNIX;
  strncpy(this->address.sun_path, this->socket_path.c_str(), 107);

  if (::bind(this->socket_fd, reinterpret_cast<struct sockaddr *>(&(this->address)), sizeof(struct sockaddr_un)) != 0) {
    return false;
  }

  if (listen(this->socket_fd, 5) != 0) {
    return false;
  }
  return true;
}

/**
 * @brief Accepts one pending client connection, if any.
 *
 * Called repeatedly by the threaded base server. Because the socket is
 * non-blocking, returns immediately; a negative value means no connection was
 * pending.
 * @return The accepted client file descriptor, or a negative value if none.
 */
int UnixDomainSocketServer::CheckForConnection() {
  socklen_t address_length = sizeof(this->address);

  int fd;
  fd = accept(this->socket_fd, reinterpret_cast<struct sockaddr *>(&(this->address)), &address_length);
  return fd;
}

/**
 * @brief Serves a single accepted connection, then closes it.
 *
 * Reads a delimiter-terminated JSON-RPC request from the socket, dispatches it
 * via ProcessRequest, appends the delimiter to the response and writes it back,
 * then closes the connection file descriptor.
 * @param connection The client socket file descriptor to service.
 */
void UnixDomainSocketServer::HandleConnection(int connection) {
  string request, response;
  StreamReader reader(DEFAULT_BUFFER_SIZE);
  reader.Read(request, connection, DEFAULT_DELIMITER_CHAR);
  this->ProcessRequest(request, response);

  response.append(1, DEFAULT_DELIMITER_CHAR);
  StreamWriter writer;
  writer.Write(response, connection);

  close(connection);
}
