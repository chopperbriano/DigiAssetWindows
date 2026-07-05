/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    windowstcpsocketserver.cpp
 * @date    17.07.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Role in DigiAsset for Windows:
 * Winsock2 implementation of the TCP JSON-RPC server connector (bundled
 * libjson-rpc-cpp dependency). This is the connector actually used by the
 * Windows node and pool-server builds to expose their JSON-RPC interface.
 * The listening socket runs its accept loop on a dedicated Windows thread;
 * each accepted client is handled on its own worker thread that reads a
 * delimiter-terminated request, dispatches it, and sends the response.
 * A file-scope initializer object drives WSAStartup/WSACleanup for the process.
 */

#include "windowstcpsocketserver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#include <iostream>
#include <sstream>
#include <string>

#include <jsonrpccpp/common/specificationparser.h>

using namespace jsonrpc;
using namespace std;

#define BUFFER_SIZE 64
#ifndef DELIMITER_CHAR
#define DELIMITER_CHAR char(0x0A)
#endif // DELIMITER_CHAR

WindowsTcpSocketServer::WindowsTcpSocketServer(const std::string &ipToBind, const unsigned int &port)
    : AbstractServerConnector(), ipToBind(ipToBind), port(port), running(false) {}

WindowsTcpSocketServer::~WindowsTcpSocketServer() {}

/**
 * @brief Creates and binds the listening socket, then spawns the accept loop.
 *
 * No-op returning false if already running. Creates a non-blocking, reusable
 * TCP socket bound to ipToBind:port, calls listen(), then starts LaunchLoop on
 * a new Windows thread. Terminates the process (ExitProcess(3)) if the thread
 * cannot be created.
 * @return true if listening started successfully, false otherwise.
 */
bool WindowsTcpSocketServer::StartListening() {
  if (!this->running) {
    // Create and bind socket here.
    // Then launch the listenning loop.
    this->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (this->socket_fd < 0) {
      return false;
    }
    unsigned long nonBlocking = 1;
    ioctlsocket(this->socket_fd, FIONBIO, &nonBlocking); // Set non blocking
    int reuseaddr = 1;
    setsockopt(this->socket_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&reuseaddr), sizeof(reuseaddr));

    /* start with a clean address structure */
    memset(&(this->address), 0, sizeof(SOCKADDR_IN));

    this->address.sin_family = AF_INET;
    this->address.sin_addr.s_addr = inet_addr(this->ipToBind.c_str());
    this->address.sin_port = htons(this->port);

    if (::bind(this->socket_fd, reinterpret_cast<SOCKADDR *>(&(this->address)), sizeof(SOCKADDR_IN)) != 0) {
      return false;
    }

    if (listen(this->socket_fd, 5) != 0) {
      return false;
    }
    // Launch listening loop there
    this->running = true;
    HANDLE ret = CreateThread(NULL, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(&(WindowsTcpSocketServer::LaunchLoop)), reinterpret_cast<LPVOID>(this), 0,
                              &(this->listenning_thread));
    if (ret == NULL) {
      ExitProcess(3);
    } else {
      CloseHandle(ret);
    }
    this->running = static_cast<bool>(ret != NULL);
    return this->running;
  } else {
    return false;
  }
}

/**
 * @brief Signals the accept loop to stop, waits for it, and closes the socket.
 *
 * Clears the running flag, blocks until the listen thread exits, then closes
 * the listening socket. Returns false if the server was not running.
 * @return true if the server was running and has been stopped.
 */
bool WindowsTcpSocketServer::StopListening() {
  if (this->running) {
    this->running = false;
    WaitForSingleObject(OpenThread(THREAD_ALL_ACCESS, FALSE, this->listenning_thread), INFINITE);
    closesocket(this->socket_fd);
    return !(this->running);
  } else {
    return false;
  }
}

/**
 * @brief Sends an RPC response over a client socket and cleanly closes it.
 *
 * Ensures the response ends with the delimiter character, writes it to the
 * client socket (whose fd is passed through addInfo), then performs a clean
 * close of the connection.
 * @param response The response payload to send.
 * @param addInfo The client socket fd, reinterpreted from a void pointer.
 * @return true if the full response was written successfully.
 */
bool WindowsTcpSocketServer::SendResponse(const string &response, void *addInfo) {
  bool result = false;
  int connection_fd = reinterpret_cast<intptr_t>(addInfo);

  string temp = response;
  if (temp.find(DELIMITER_CHAR) == string::npos) {
    temp.append(1, DELIMITER_CHAR);
  }
  if (DELIMITER_CHAR != '\n') {
    char eot = DELIMITER_CHAR;
    string toSend = temp.substr(0, toSend.find_last_of('\n'));
    toSend += eot;
    result = this->WriteToSocket(connection_fd, toSend);
  } else {
    result = this->WriteToSocket(connection_fd, temp);
  }
  CleanClose(connection_fd);
  return result;
}

/**
 * @brief Thread entry point for the accept loop.
 *
 * Casts lp_data back to the owning server instance and runs its ListenLoop
 * until stopped.
 * @param lp_data The WindowsTcpSocketServer instance pointer.
 * @return Always 0.
 */
DWORD WINAPI WindowsTcpSocketServer::LaunchLoop(LPVOID lp_data) {
  WindowsTcpSocketServer *instance = reinterpret_cast<WindowsTcpSocketServer *>(lp_data);
  ;
  instance->ListenLoop();
  CloseHandle(GetCurrentThread());
  return 0; // DO NOT USE ExitThread function here! ExitThread does not call
            // destructors for allocated objects and therefore it would lead to
            // a memory leak.
}

/**
 * @brief Accept loop: dispatches each new client to its own worker thread.
 *
 * Runs while running is true. accept()s connections (non-blocking, sleeping
 * briefly when none are pending), switches each accepted socket back to
 * blocking mode, and spawns a GenerateResponse thread with a heap-allocated
 * parameter block. If the worker thread cannot be created, the parameters are
 * freed and the connection is cleanly closed.
 */
void WindowsTcpSocketServer::ListenLoop() {
  while (this->running) {
    SOCKET connection_fd = INVALID_SOCKET;
    SOCKADDR_IN connection_address;
    memset(&connection_address, 0, sizeof(SOCKADDR_IN));
    int address_length = sizeof(connection_address);
    if ((connection_fd = accept(this->socket_fd, reinterpret_cast<SOCKADDR *>(&connection_address), &address_length)) != INVALID_SOCKET) {
      unsigned long nonBlocking = 0;
      ioctlsocket(connection_fd, FIONBIO, &nonBlocking); // Set blocking
      DWORD client_thread;
      struct GenerateResponseParameters *params = new struct GenerateResponseParameters();
      params->instance = this;
      params->connection_fd = connection_fd;
      HANDLE ret = CreateThread(NULL, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(&(WindowsTcpSocketServer::GenerateResponse)),
                                reinterpret_cast<LPVOID>(params), 0, &client_thread);
      if (ret == NULL) {
        delete params;
        params = NULL;
        CleanClose(connection_fd);
      } else {
        CloseHandle(ret);
      }
    } else {
      Sleep(2.5);
    }
  }
}

/**
 * @brief Per-client worker thread: reads a request, dispatches, replies.
 *
 * Takes ownership of the heap-allocated GenerateResponseParameters (deleting
 * them), reads from the client socket until the delimiter is seen, runs the
 * request through ProcessRequest, and sends the response (which also closes the
 * connection).
 * @param lp_data Pointer to a GenerateResponseParameters block.
 * @return Always 0.
 */
DWORD WINAPI WindowsTcpSocketServer::GenerateResponse(LPVOID lp_data) {
  struct GenerateResponseParameters *params = reinterpret_cast<struct GenerateResponseParameters *>(lp_data);
  WindowsTcpSocketServer *instance = params->instance;
  int connection_fd = params->connection_fd;
  delete params;
  params = NULL;
  int nbytes = 0;
  char buffer[BUFFER_SIZE];
  memset(&buffer, 0, BUFFER_SIZE);
  string request = "";
  do { // The client sends its json formatted request and a delimiter request.
    nbytes = recv(connection_fd, buffer, BUFFER_SIZE, 0);
    if (nbytes == -1) {
      instance->CleanClose(connection_fd);
    } else {
      request.append(buffer, nbytes);
    }
  } while (request.find(DELIMITER_CHAR) == string::npos);
  std::string response;
  instance->ProcessRequest(request, response);
  instance->SendResponse(response, reinterpret_cast<void *>(connection_fd));
  CloseHandle(GetCurrentThread());
  return 0; // DO NOT USE ExitThread function here! ExitThread does not call
            // destructors for allocated objects and therefore it would lead to
            // a memory leak.
}

/**
 * @brief Writes a full message to a socket, looping over partial sends.
 *
 * Repeatedly calls send() until the whole buffer is written or an error occurs;
 * on error it cleanly closes the socket.
 * @param fd The client socket file descriptor.
 * @param toWrite The message to send.
 * @return true if the message was fully written without error.
 */
bool WindowsTcpSocketServer::WriteToSocket(const SOCKET &fd, const string &toWrite) {
  bool fullyWritten = false;
  bool errorOccured = false;
  string toSend = toWrite;
  do {
    unsigned long byteWritten = send(fd, toSend.c_str(), toSend.size(), 0);
    if (byteWritten < 0) {
      errorOccured = true;
      CleanClose(fd);
    } else if (byteWritten < toSend.size()) {
      int len = toSend.size() - byteWritten;
      toSend = toSend.substr(byteWritten + sizeof(char), len);
    } else
      fullyWritten = true;
  } while (!fullyWritten && !errorOccured);
  return fullyWritten && !errorOccured;
}

/**
 * @brief Waits (up to timeout ms) for the client to close the TCP session.
 *
 * Polls recv() once per millisecond so the server side avoids lingering in
 * TIME_WAIT, which under many connections could exhaust sockets.
 * @param fd The client socket file descriptor.
 * @param timeout Maximum wait in milliseconds.
 * @return true if the client closed (or was polled at least once), false if it
 *         had already closed before the first poll.
 */
bool WindowsTcpSocketServer::WaitClientClose(const SOCKET &fd, const int &timeout) {
  bool ret = false;
  int i = 0;
  while ((recv(fd, NULL, 0, 0) != 0) && i < timeout) {
    Sleep(1);
    ++i;
    ret = true;
  }

  return ret;
}

/**
 * @brief Force-closes a socket by resetting it (SO_LINGER with 0 timeout).
 *
 * Sends a TCP RST instead of a graceful FIN so the local side skips TIME_WAIT.
 * @param fd The client socket file descriptor.
 * @return 0 on success, or the failing setsockopt/closesocket return value.
 */
int WindowsTcpSocketServer::CloseByReset(const SOCKET &fd) {
  struct linger so_linger;
  so_linger.l_onoff = 1;
  so_linger.l_linger = 0;

  int ret = setsockopt(fd, SOL_SOCKET, SO_LINGER, reinterpret_cast<char *>(&so_linger), sizeof(so_linger));
  if (ret != 0)
    return ret;

  return closesocket(fd);
}

/**
 * @brief Cleanly closes a connection while avoiding TIME_WAIT.
 *
 * Waits for the client to close first (WaitClientClose); if it does, closes
 * normally, otherwise resets the socket via CloseByReset.
 * @param fd The client socket file descriptor.
 * @return The closesocket()/CloseByReset() return value.
 */
int WindowsTcpSocketServer::CleanClose(const SOCKET &fd) {
  if (WaitClientClose(fd)) {
    return closesocket(fd);
  } else {
    return CloseByReset(fd);
  }
}

// This is inspired from SFML to manage Winsock initialization. Thanks to them!
// ( http://www.sfml-dev.org/ ).
// RAII helper: a single file-scope instance (serverGlobalInitializer) calls
// WSAStartup at process load and WSACleanup at unload, so Winsock is ready
// before any socket in this connector is used.
struct ServerSocketInitializer {
  ServerSocketInitializer() {
    WSADATA init;
    if (WSAStartup(MAKEWORD(2, 2), &init) != 0) {
      JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "An issue occurred while WSAStartup executed.");
    }
  }

  ~ServerSocketInitializer() {
    if (WSACleanup() != 0) {
      cerr << "An issue occurred while WSAClean executed." << endl;
    }
  }
};

struct ServerSocketInitializer serverGlobalInitializer;
