/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    filedescriptorclient.h
 * @date    26.10.2016
 * @author  Jean-Daniel Michaud <jean.daniel.michaud@gmail.com>
 * @license See attached LICENSE.txt
 ************************************************************************/

// File role: Declares FileDescriptorClient, an IClientConnector that carries
// JSON-RPC requests/responses over a pair of raw POSIX file descriptors (e.g.
// pipes or sockets) instead of HTTP. Part of libjson-rpc-cpp's client
// transports; usable where the peer RPC endpoint is reachable via fds.

#ifndef JSONRPC_CPP_FILEDESCRITPTORCLIENT_H_
#define JSONRPC_CPP_FILEDESCRITPTORCLIENT_H_

#include "../iclientconnector.h"
#include <jsonrpccpp/common/exception.h>

namespace jsonrpc {
  // Connector that writes each request to `outputfd` and reads the reply from
  // `inputfd`, using the library's delimiter to frame messages.
  class FileDescriptorClient : public IClientConnector {
  public:
    // inputfd is read for responses, outputfd is written with requests.
    FileDescriptorClient(int inputfd, int outputfd);
    virtual ~FileDescriptorClient();
    // Writes `message` (plus delimiter) to outputfd and reads the framed reply
    // into `result`. Throws JsonRpcException on write/read failure or if the
    // input fd is not readable.
    virtual void SendRPCMessage(const std::string &message, std::string &result);

  protected:
    int inputfd;
    int outputfd;

    // Returns true if `fd`'s open mode allows reading (O_RDONLY or O_RDWR).
    bool IsReadable(int fd);
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_FILEDESCRITPTORCLIENT_H_ */
