/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    unixdomainsocketserver.h
 * @date    07.05.2015
 * @author  Alexandre Poirot <alexandre.poirot@legrand.fr>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Role in DigiAsset for Windows:
 * Declares the UnixDomainSocketServer JSON-RPC connector (bundled
 * libjson-rpc-cpp dependency). It derives from AbstractThreadedServer and
 * serves RPC requests over a POSIX Unix-domain socket. This connector is
 * POSIX-only (it pulls in <sys/un.h>, <pthread.h>, etc.) and is not compiled
 * into the Windows node/pool build, which uses WindowsTcpSocketServer instead.
 */

#ifndef JSONRPC_CPP_UNIXDOMAINSOCKETSERVERCONNECTOR_H_
#define JSONRPC_CPP_UNIXDOMAINSOCKETSERVERCONNECTOR_H_

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../common/streamreader.h"
#include "../../common/streamwriter.h"
#include "../abstractthreadedserver.h"

namespace jsonrpc {
  /**
   * This class provides an embedded Unix Domain Socket Server,to handle incoming
   * Requests.
   */
  class UnixDomainSocketServer : public AbstractThreadedServer {
  public:
    /**
     * @brief UnixDomainSocketServer, constructor for the included
     * UnixDomainSocketServer
     * @param socket_path, a string containing the path to the unix socket
     */
    UnixDomainSocketServer(const std::string &socket_path, size_t threads = 1);
    virtual ~UnixDomainSocketServer();

    /** Creates, binds and listens on the Unix-domain socket; see .cpp. */
    virtual bool InitializeListener();
    /** Accepts a pending client (non-blocking); returns its fd or <0. */
    virtual int CheckForConnection();
    /** Reads one request, dispatches it, writes the reply, closes the fd. */
    virtual void HandleConnection(int connection);

  protected:
    std::string socket_path;     /*!< Filesystem path of the listening socket */
    int socket_fd;               /*!< Listening socket fd (-1 when not open) */
    struct sockaddr_un address;  /*!< Bind/accept address for the socket */
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_HTTPSERVERCONNECTOR_H_ */
