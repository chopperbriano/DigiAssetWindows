/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    unixdomainsocketclient.cpp
 * @date    11.05.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************
 * Role in DigiAsset for Windows: implements the AF_UNIX JSON-RPC client
 * connector. Each call opens a socket, connects to the configured path,
 * writes the delimited request, reads the delimited response, and closes
 * the socket. POSIX-only (relies on <sys/un.h>); part of the bundled
 * libjson-rpc-cpp client library and not exercised on the Windows build.
 ************************************************************************/

#include "unixdomainsocketclient.h"
#include "../../common/sharedconstants.h"
#include "../../common/streamreader.h"
#include "../../common/streamwriter.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace jsonrpc;
using namespace std;

UnixDomainSocketClient::UnixDomainSocketClient(const std::string &path) : path(path) {}

UnixDomainSocketClient::~UnixDomainSocketClient() {}

// Perform one JSON-RPC round-trip over a Unix domain socket:
//   1. create an AF_UNIX/SOCK_STREAM socket and connect to this->path;
//   2. append DEFAULT_DELIMITER_CHAR to message and write it via StreamWriter;
//   3. read the reply into result via StreamReader up to the same delimiter;
//   4. close the socket.
// message is the request; the response is returned in result. Throws
// JsonRpcException (ERROR_CLIENT_CONNECTOR) on socket create/connect/write/read
// failure, always closing the fd first. Note: sun_path is copied with a fixed
// 107-byte cap, so longer paths are silently truncated.
void UnixDomainSocketClient::SendRPCMessage(const std::string &message, std::string &result) {
  sockaddr_un address;
  int socket_fd;
  socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not create unix domain socket");
  }
  memset(&address, 0, sizeof(sockaddr_un));

  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, this->path.c_str(), 107);

  if (connect(socket_fd, (struct sockaddr *)&address, sizeof(sockaddr_un)) != 0) {
    close(socket_fd);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not connect to: " + this->path);
  }

  StreamWriter writer;
  string toSend = message + DEFAULT_DELIMITER_CHAR;
  if (!writer.Write(toSend, socket_fd)) {
    close(socket_fd);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not write request");
  }

  StreamReader reader(DEFAULT_BUFFER_SIZE);
  if (!reader.Read(result, socket_fd, DEFAULT_DELIMITER_CHAR)) {
    close(socket_fd);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not read response");
  }
  close(socket_fd);
}
