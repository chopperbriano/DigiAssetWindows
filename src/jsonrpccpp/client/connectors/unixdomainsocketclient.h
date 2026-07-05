/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    unixdomainsocketclient.h
 * @date    11.05.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************
 * Role in DigiAsset for Windows: declares the JSON-RPC client connector
 * that talks to a peer over a Unix domain (AF_UNIX) socket instead of TCP.
 * Part of the bundled libjson-rpc-cpp client library. This is a POSIX-only
 * transport (its .cpp uses <sys/un.h>/<sys/socket.h>) and is not used on the
 * Windows node build, where the TCP/HTTP connectors are used instead.
 ************************************************************************/

#ifndef JSONRPC_CPP_UNIXDOMAINSOCKETCLIENT_H_
#define JSONRPC_CPP_UNIXDOMAINSOCKETCLIENT_H_

#include "../iclientconnector.h"
#include <jsonrpccpp/common/exception.h>

namespace jsonrpc {
  // IClientConnector implementation that sends each JSON-RPC request over a
  // freshly opened Unix domain socket at a filesystem path and reads back the
  // response. One connect/write/read/close cycle per SendRPCMessage call.
  class UnixDomainSocketClient : public IClientConnector {
  public:
    // Construct with the filesystem path of the Unix socket to connect to.
    UnixDomainSocketClient(const std::string &path);
    virtual ~UnixDomainSocketClient();
    // Send one request and receive the reply; see .cpp for error handling.
    virtual void SendRPCMessage(const std::string &message, std::string &result);

  protected:
    std::string path; /*!< Filesystem path of the target Unix domain socket */
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_UNIXDOMAINSOCKETCLIENT_H_ */
