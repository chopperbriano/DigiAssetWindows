/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    abstractserverconnector.cpp
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: base class for the RPC transport layer
// (e.g. HTTP/TCP connector) that carries JSON-RPC traffic for the node/pool.
// It holds a pointer to the client-connection handler (the protocol handler)
// and forwards received request bodies to it, returning the produced response.
// Subclasses implement the actual listen/accept loop.

#include "abstractserverconnector.h"
#include <cstdlib>
#include <jsonrpccpp/common/specificationwriter.h>

using namespace std;
using namespace jsonrpc;

AbstractServerConnector::AbstractServerConnector() { this->handler = NULL; }

AbstractServerConnector::~AbstractServerConnector() {}

// Hand a received request body to the bound handler and receive the serialized
// response. No-op if no handler has been set.
void AbstractServerConnector::ProcessRequest(const string &request, string &response) {
  if (this->handler != NULL) {
    this->handler->HandleRequest(request, response);
  }
}

void AbstractServerConnector::SetHandler(IClientConnectionHandler *handler) { this->handler = handler; }

IClientConnectionHandler *AbstractServerConnector::GetHandler() { return this->handler; }
