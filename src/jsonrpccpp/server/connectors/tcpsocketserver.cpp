/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    tcpsocketserver.cpp
 * @date    17.07.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * ROLE IN DIGIASSET FOR WINDOWS:
 * Cross-platform facade for the JSON-RPC TCP socket server connector from
 * vendored libjson-rpc-cpp. TcpSocketServer owns a platform-specific backend
 * (WindowsTcpSocketServer on this fork, LinuxTcpSocketServer elsewhere,
 * selected at compile time) and forwards StartListening/StopListening to it.
 * This is the connector the node/pool can use to expose their JSON-RPC API
 * over a raw TCP socket.
 */

#include "tcpsocketserver.h"
#ifdef _WIN32
#include "windowstcpsocketserver.h"
#else
#include "linuxtcpsocketserver.h"
#endif
#include <string>

using namespace jsonrpc;
using namespace std;

/**
 * @brief Constructs the facade and instantiates the platform-appropriate
 *        backend (Windows or Linux TCP socket server) bound to ipToBind:port.
 * @param ipToBind IPv4 address to bind. @param port TCP port to listen on.
 */
TcpSocketServer::TcpSocketServer(const std::string &ipToBind, const unsigned int &port) : AbstractServerConnector() {
#ifdef _WIN32
  this->realSocket = new WindowsTcpSocketServer(ipToBind, port);
#else
  this->realSocket = new LinuxTcpSocketServer(ipToBind, port);
#endif
}

TcpSocketServer::~TcpSocketServer() { delete this->realSocket; }

/**
 * @brief Propagates this facade's request handler to the backend and starts it listening.
 * @returns The backend's StartListening result, or false if no backend exists.
 */
bool TcpSocketServer::StartListening() {
  if (this->realSocket != NULL) {
    this->realSocket->SetHandler(this->GetHandler());
    return this->realSocket->StartListening();
  } else
    return false;
}

/**
 * @brief Stops the backend connector from listening.
 * @returns The backend's StopListening result, or false if no backend exists.
 */
bool TcpSocketServer::StopListening() {
  if (this->realSocket != NULL)
    return this->realSocket->StopListening();
  else
    return false;
}
