/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    tcpsocketclient.cpp
 * @date    17.07.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************
 * Role in DigiAsset for Windows: implements the OS-agnostic TcpSocketClient
 * facade. At construction it picks the correct platform backend
 * (WindowsTcpSocketClient on _WIN32, LinuxTcpSocketClient otherwise) and
 * forwards every SendRPCMessage call to it. Part of the bundled
 * libjson-rpc-cpp TCP client connector.
 ************************************************************************/

#include "tcpsocketclient.h"

#ifdef _WIN32
#include "windowstcpsocketclient.h"
#else
#include "linuxtcpsocketclient.h"
#endif

using namespace jsonrpc;
using namespace std;

// Construct the facade: allocate the platform-specific backend for the
// given IPv4 address and port and store it in realSocket. No connection is
// opened here; the backend connects lazily on the first SendRPCMessage.
TcpSocketClient::TcpSocketClient(const std::string &ipToConnect, const unsigned int &port) {
#ifdef _WIN32
  this->realSocket = new WindowsTcpSocketClient(ipToConnect, port);
#else
  this->realSocket = new LinuxTcpSocketClient(ipToConnect, port);
#endif
}

// Destructor: free the platform-specific backend created in the constructor.
TcpSocketClient::~TcpSocketClient() { delete this->realSocket; }

// Forward one JSON-RPC request/response round-trip to the backend. message is
// the outbound request; the reply is written into result. Any socket error is
// surfaced by the backend as a JsonRpcException. No-op if no backend exists.
void TcpSocketClient::SendRPCMessage(const std::string &message, std::string &result) {
  if (this->realSocket != NULL) {
    this->realSocket->SendRPCMessage(message, result);
  }
}
