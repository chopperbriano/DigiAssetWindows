/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    linuxtcpsocketclient.cpp
 * @date    17.10.2016
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************/

// ---------------------------------------------------------------------------
// Role in DigiAsset for Windows:
//   Implements LinuxTcpSocketClient, the POSIX TCP-socket transport for the
//   bundled libjson-rpc-cpp JSON-RPC client. It resolves a host/IP, opens a
//   TCP connection, writes a delimiter-framed JSON-RPC request and reads the
//   framed response. Being built on the POSIX sockets API (<sys/socket.h>,
//   <netdb.h>, unistd), it is a Linux/UNIX-only connector and is not part of
//   the Windows node or pool-server build; it ships to keep the vendored
//   library source complete.
// ---------------------------------------------------------------------------

#include "linuxtcpsocketclient.h"
#include "../../common/sharedconstants.h"
#include "../../common/streamreader.h"
#include "../../common/streamwriter.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace jsonrpc;
using namespace std;

LinuxTcpSocketClient::LinuxTcpSocketClient(const std::string &hostToConnect, const unsigned int &port) : hostToConnect(hostToConnect), port(port) {}

LinuxTcpSocketClient::~LinuxTcpSocketClient() {}

/**
 * @brief Sends a JSON-RPC request over a fresh TCP connection and returns the reply.
 *
 * Connects to the configured host/port, writes message terminated by
 * DEFAULT_DELIMITER_CHAR, reads the response up to the same delimiter into
 * result, then closes the socket.
 * @param message The JSON-RPC request to send.
 * @param result  Receives the raw JSON-RPC response read from the server.
 * @throw JsonRpcException on connect, write or read failure.
 */
void LinuxTcpSocketClient::SendRPCMessage(const std::string &message, std::string &result) {
  int socket_fd = this->Connect();

  StreamWriter writer;
  string toSend = message + DEFAULT_DELIMITER_CHAR;
  if (!writer.Write(toSend, socket_fd)) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not write request");
  }

  StreamReader reader(DEFAULT_BUFFER_SIZE);
  if (!reader.Read(result, socket_fd, DEFAULT_DELIMITER_CHAR)) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not read response");
  }
  close(socket_fd);
}

/**
 * @brief Establishes a TCP connection to the host/port from the constructor.
 *
 * If hostToConnect is already a dotted-quad IPv4 address it connects directly.
 * Otherwise it resolves the hostname via getaddrinfo() and tries each returned
 * IPv4 address until one connects.
 * @returns A file descriptor for the connected socket.
 * @throw JsonRpcException if the hostname cannot be resolved or no address accepts the connection.
 */
int LinuxTcpSocketClient::Connect() {
  if (this->IsIpv4Address(this->hostToConnect)) {
    return this->Connect(this->hostToConnect, this->port);
  } else // We were given a hostname
  {
    struct addrinfo *result = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char port[6];
    snprintf(port, 6, "%d", this->port);
    int retval = getaddrinfo(this->hostToConnect.c_str(), port, &hints, &result);
    if (retval != 0)
      throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not resolve hostname.");
    bool foundValidIp = false;
    int socket_fd;
    for (struct addrinfo *temp = result; (temp != NULL) && !foundValidIp; temp = temp->ai_next) {
      if (temp->ai_family == AF_INET) {
        try {
          sockaddr_in *sock = reinterpret_cast<sockaddr_in *>(temp->ai_addr);
          socket_fd = this->Connect(inet_ntoa(sock->sin_addr), ntohs(sock->sin_port));
          foundValidIp = true;
        } catch (const JsonRpcException &e) {
          foundValidIp = false;
          socket_fd = -1;
        } catch (void *p) {
          foundValidIp = false;
          socket_fd = -1;
        }
      }
    }

    if (!foundValidIp)
      throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Hostname resolved but connection was refused on the given port.");

    return socket_fd;
  }
}

/**
 * @brief Creates a socket and connects it to a specific IPv4 address and port.
 *
 * On socket() or connect() failure the errno is translated to a message via
 * strerror() for known error codes and the socket (if opened) is closed.
 * @param ip   Dotted-quad IPv4 address to connect to.
 * @param port TCP port to connect to.
 * @returns A file descriptor for the connected socket.
 * @throw JsonRpcException if socket creation or connection fails.
 */
int LinuxTcpSocketClient::Connect(const string &ip, const int &port) {
  sockaddr_in address;
  int socket_fd;
  socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    string message = "socket() failed";
    int err = errno;
    switch (err) {
    case EACCES:
    case EAFNOSUPPORT:
    case EINVAL:
    case EMFILE:
    case ENOBUFS:
    case ENOMEM:
    case EPROTONOSUPPORT:
      message = strerror(err);
      break;
    }
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, message);
  }
  memset(&address, 0, sizeof(sockaddr_in));

  address.sin_family = AF_INET;
  inet_aton(ip.c_str(), &(address.sin_addr));
  address.sin_port = htons(port);

  if (connect(socket_fd, (struct sockaddr *)&address, sizeof(sockaddr_in)) != 0) {
    string message = "connect() failed";
    int err = errno;
    switch (err) {
    case EACCES:
    case EPERM:
    case EADDRINUSE:
    case EAFNOSUPPORT:
    case EAGAIN:
    case EALREADY:
    case EBADF:
    case ECONNREFUSED:
    case EFAULT:
    case EINPROGRESS:
    case EINTR:
    case EISCONN:
    case ENETUNREACH:
    case ENOTSOCK:
    case ETIMEDOUT:
      message = strerror(err);
      break;
    }
    close(socket_fd);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, message);
  }
  return socket_fd;
}

/**
 * @brief Tests whether a string is a valid dotted-quad IPv4 address.
 * @param ip The string to test.
 * @returns true if inet_aton() parses ip as an IPv4 address, false otherwise.
 */
bool LinuxTcpSocketClient::IsIpv4Address(const std::string &ip) {
  struct in_addr addr;
  return (inet_aton(ip.c_str(), &addr) != 0);
}
