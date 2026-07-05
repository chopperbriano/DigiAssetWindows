/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    windowstcpsocketclient.cpp
 * @date    17.07.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in the node/pool: this is the Windows (Winsock2) transport that the
// embedded json-rpc client uses to talk to the local DigiByte Core daemon over
// a raw TCP socket. Requests built by RpcProtocolClient are written to the
// socket here, and the server's newline-delimited JSON reply is read back.
// Every chain query the node/pool makes against digibyted (getblock, gettx,
// etc.) ultimately flows through SendRPCMessage in this file.

#include "windowstcpsocketclient.h"
#include <cstdlib>
#include <iostream>
#include <string.h>
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x501
#include <ws2tcpip.h>

#define BUFFER_SIZE 64
#ifndef DELIMITER_CHAR
#define DELIMITER_CHAR char(0x0A)
#endif // DELIMITER_CHAR

using namespace jsonrpc;
using namespace std;

WindowsTcpSocketClient::WindowsTcpSocketClient(const std::string &hostToConnect, const unsigned int &port) : hostToConnect(hostToConnect), port(port) {}

WindowsTcpSocketClient::~WindowsTcpSocketClient() {}

/**
 * @brief Sends one JSON-RPC request over a fresh TCP connection and reads the reply.
 *
 * Opens a socket via Connect(), writes the entire @p message (looping until every
 * byte is sent, re-slicing the string on partial writes), then reads BUFFER_SIZE
 * chunks into @p result until the delimiter byte (0x0A / newline) is seen. Each
 * call uses a new connection, which is closed before returning. On any Winsock
 * send()/recv() failure the socket is closed and a JsonRpcException carrying the
 * decoded Winsock error text is thrown.
 *
 * @param message The serialized JSON-RPC request to transmit.
 * @param result  Appended with the raw server response (including the delimiter).
 * @throw JsonRpcException on socket send/recv errors (code ERROR_CLIENT_CONNECTOR).
 */
void WindowsTcpSocketClient::SendRPCMessage(const std::string &message, std::string &result) throw(JsonRpcException) {
  SOCKET socket_fd = this->Connect();
  char buffer[BUFFER_SIZE];
  bool fullyWritten = false;
  string toSend = message;
  do {
    int byteWritten = send(socket_fd, toSend.c_str(), toSend.size(), 0);
    if (byteWritten == -1) {
      string message = "send() failed";
      int err = WSAGetLastError();
      switch (err) {
      case WSANOTINITIALISED:
      case WSAENETDOWN:
      case WSAEACCES:
      case WSAEINTR:
      case WSAEINPROGRESS:
      case WSAEFAULT:
      case WSAENETRESET:
      case WSAENOBUFS:
      case WSAENOTCONN:
      case WSAENOTSOCK:
      case WSAEOPNOTSUPP:
      case WSAESHUTDOWN:
      case WSAEWOULDBLOCK:
      case WSAEMSGSIZE:
      case WSAEHOSTUNREACH:
      case WSAEINVAL:
      case WSAECONNABORTED:
      case WSAECONNRESET:
      case WSAETIMEDOUT:
        message = GetErrorMessage(err);
        break;
      }
      closesocket(socket_fd);
      throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, message);
    } else if (static_cast<unsigned int>(byteWritten) < toSend.size()) {
      int len = toSend.size() - byteWritten;
      toSend = toSend.substr(byteWritten + sizeof(char), len);
    } else
      fullyWritten = true;
  } while (!fullyWritten);

  do {
    int nbytes = recv(socket_fd, buffer, BUFFER_SIZE, 0);
    if (nbytes == -1) {
      string message = "recv() failed";
      int err = WSAGetLastError();
      switch (err) {
      case WSANOTINITIALISED:
      case WSAENETDOWN:
      case WSAEFAULT:
      case WSAENOTCONN:
      case WSAEINTR:
      case WSAEINPROGRESS:
      case WSAENETRESET:
      case WSAENOTSOCK:
      case WSAEOPNOTSUPP:
      case WSAESHUTDOWN:
      case WSAEWOULDBLOCK:
      case WSAEMSGSIZE:
      case WSAEINVAL:
      case WSAECONNABORTED:
      case WSAETIMEDOUT:
      case WSAECONNRESET:
        message = GetErrorMessage(err);
        break;
      }
      closesocket(socket_fd);
      throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, message);
    } else {
      string tmp;
      tmp.append(buffer, nbytes);
      result.append(buffer, nbytes);
    }

  } while (result.find(DELIMITER_CHAR) == string::npos);

  closesocket(socket_fd);
}

/**
 * @brief Turns a Winsock2 error code into a human-readable string.
 *
 * Wraps FormatMessage() to look up the system description for error code @p e,
 * copying it into a std::string and freeing the buffer FormatMessage allocated.
 *
 * @param e A Winsock2 error value (e.g. from WSAGetLastError()).
 * @return The system's text description of the error.
 */
string WindowsTcpSocketClient::GetErrorMessage(const int &e) {
  LPVOID lpMsgBuf;
  lpMsgBuf = (LPVOID) "Unknown error";
  FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPTSTR)&lpMsgBuf, 0, NULL);
  string message(static_cast<char *>(lpMsgBuf));
  LocalFree(lpMsgBuf);
  return message;
}

/**
 * @brief Resolves the configured host and returns a connected socket.
 *
 * If hostToConnect is already a dotted IPv4 literal, connects to it directly.
 * Otherwise treats it as a DNS hostname: resolves it with getaddrinfo() and
 * tries each returned IPv4 address in turn, returning the first that connects.
 *
 * @return A connected socket descriptor.
 * @throw JsonRpcException if the hostname cannot be resolved or no resolved
 *        address accepts the connection (code ERROR_CLIENT_CONNECTOR).
 */
SOCKET WindowsTcpSocketClient::Connect() throw(JsonRpcException) {
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
    itoa(this->port, port, 10);
    DWORD retval = getaddrinfo(this->hostToConnect.c_str(), port, &hints, &result);
    if (retval != 0)
      throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not resolve hostname.");

    bool foundValidIp = false;
    SOCKET socket_fd = INVALID_SOCKET;
    for (struct addrinfo *temp = result; (temp != NULL) && !foundValidIp; temp = temp->ai_next) {
      if (temp->ai_family == AF_INET) {
        try {
          SOCKADDR_IN *sock = reinterpret_cast<SOCKADDR_IN *>(temp->ai_addr);
          socket_fd = this->Connect(inet_ntoa(sock->sin_addr), ntohs(sock->sin_port));
          foundValidIp = true;
        } catch (const JsonRpcException &e) {
          foundValidIp = false;
          socket_fd = INVALID_SOCKET;
        } catch (void *p) {
          foundValidIp = false;
          socket_fd = INVALID_SOCKET;
        }
      }
    }

    if (!foundValidIp) {
      closesocket(socket_fd);
      throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Hostname resolved but connection was refused on the given port.");
    }

    return socket_fd;
  }
}

/**
 * @brief Creates a TCP socket and connects it to a specific IPv4 address/port.
 *
 * @param ip   Dotted IPv4 address to connect to.
 * @param port TCP port to connect to.
 * @return A connected socket descriptor.
 * @throw JsonRpcException if socket() or connect() fails; the socket is closed
 *        and the decoded Winsock error text is attached (ERROR_CLIENT_CONNECTOR).
 */
SOCKET
WindowsTcpSocketClient::Connect(const string &ip, const int &port) throw(JsonRpcException) {
  SOCKADDR_IN address;
  SOCKET socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == INVALID_SOCKET) {
    string message = "socket() failed";
    int err = WSAGetLastError();
    switch (err) {
    case WSANOTINITIALISED:
    case WSAENETDOWN:
    case WSAEAFNOSUPPORT:
    case WSAEINPROGRESS:
    case WSAEMFILE:
    case WSAEINVAL:
    case WSAEINVALIDPROVIDER:
    case WSAEINVALIDPROCTABLE:
    case WSAENOBUFS:
    case WSAEPROTONOSUPPORT:
    case WSAEPROTOTYPE:
    case WSAEPROVIDERFAILEDINIT:
    case WSAESOCKTNOSUPPORT:
      message = GetErrorMessage(err);
      break;
    }
    closesocket(socket_fd);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, message);
  }
  memset(&address, 0, sizeof(SOCKADDR_IN));

  address.sin_family = AF_INET;
  address.sin_addr.s_addr = inet_addr(ip.c_str());
  address.sin_port = htons(port);
  if (connect(socket_fd, reinterpret_cast<SOCKADDR *>(&address), sizeof(SOCKADDR_IN)) != 0) {
    string message = "connect() failed";
    int err = WSAGetLastError();
    switch (err) {
    case WSANOTINITIALISED:
    case WSAENETDOWN:
    case WSAEADDRINUSE:
    case WSAEINTR:
    case WSAEINPROGRESS:
    case WSAEALREADY:
    case WSAEADDRNOTAVAIL:
    case WSAEAFNOSUPPORT:
    case WSAECONNREFUSED:
    case WSAEFAULT:
    case WSAEINVAL:
    case WSAEISCONN:
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAENOBUFS:
    case WSAENOTSOCK:
    case WSAETIMEDOUT:
    case WSAEWOULDBLOCK:
    case WSAEACCES:
      message = GetErrorMessage(err);
      break;
    }
    closesocket(socket_fd);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, message);
  }
  return socket_fd;
}

/**
 * @brief Reports whether a string is a valid dotted IPv4 literal.
 *
 * Uses inet_addr(); a value other than INADDR_NONE means it parsed as IPv4.
 *
 * @param ip Candidate address string.
 * @return true if @p ip is a well-formed IPv4 address, false otherwise.
 */
bool WindowsTcpSocketClient::IsIpv4Address(const std::string &ip) { return (inet_addr(ip.c_str()) != INADDR_NONE); }

// This is inspired from SFML to manage Winsock initialization. Thanks to them!
// ( http://www.sfml-dev.org/ ).
//
// RAII guard whose single global instance initializes Winsock (WSAStartup) at
// program load and tears it down (WSACleanup) at exit, so socket calls in this
// file work without every caller managing Winsock lifetime.
struct ClientSocketInitializer {
  ClientSocketInitializer()

  {
    WSADATA init;
    if (WSAStartup(MAKEWORD(2, 2), &init) != 0) {
      throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "An issue occurred while WSAStartup executed.");
    }
  }

  ~ClientSocketInitializer()

  {
    if (WSACleanup() != 0) {
      cerr << "An issue occurred while WSAClean executed." << endl;
    }
  }
};

struct ClientSocketInitializer clientGlobalInitializer;
