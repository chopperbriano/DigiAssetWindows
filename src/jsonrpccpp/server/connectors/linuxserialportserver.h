/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    linuxserialportserver.h
 * @date    01.11.2019
 * @author  Billy Araujo
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * ROLE IN DIGIASSET FOR WINDOWS:
 * Part of the vendored libjson-rpc-cpp library that provides the JSON-RPC
 * transport layer used by the node and pool server. This header declares a
 * server connector that accepts JSON-RPC requests over a Linux serial port
 * (POSIX file descriptor + select()). It is a Linux/UNIX-only implementation
 * and is compiled out of this Windows fork (the Windows build uses the
 * Windows socket connectors instead); it is retained only for upstream
 * parity. Each accepted connection is serviced on its own worker thread by
 * the AbstractThreadedServer base class.
 */

#ifndef JSONRPC_CPP_LINUXSERIALPORTSERVERCONNECTOR_H_
#define JSONRPC_CPP_LINUXSERIALPORTSERVERCONNECTOR_H_

#include "../../common/streamreader.h"
#include "../../common/streamwriter.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "../abstractthreadedserver.h"

namespace jsonrpc {
  /**
   * This class is the Linux/UNIX implementation of TCPSocketServer.
   * It uses the POSIX socket API and POSIX thread API to performs its job.
   * Each client request is handled in a new thread.
   */
  class LinuxSerialPortServer : public AbstractThreadedServer {
  public:
    /**
     * @brief LinuxSerialPortServer, constructor of the Linux/UNIX
     * implementation of class TcpSocketServer
     * @param deviceName The ipv4 address on which the server should
     */
    LinuxSerialPortServer(const std::string &deviceName, size_t threads = 1);

    virtual ~LinuxSerialPortServer();

    /**
     * @brief Opens the serial device and prepares it to receive requests.
     * @returns true if the device was opened successfully, false otherwise.
     */
    virtual bool InitializeListener();
    /**
     * @brief Waits (via select on the serial fd) for an incoming request.
     * @returns The file descriptor to service, or a negative value on timeout/error.
     */
    virtual int CheckForConnection();
    /**
     * @brief Reads one JSON-RPC request from the given fd, dispatches it to the
     *        handler, and writes the response back over the serial link.
     * @param connection The serial file descriptor to read from / write to.
     */
    virtual void HandleConnection(int connection);

  protected:
    std::string deviceName; ///< Path of the serial device to open (e.g. /dev/ttyS0).
    int serial_fd;          ///< Open file descriptor for the serial device.

    StreamReader reader;
    StreamWriter writer;

    // For select operation
    fd_set read_fds;
    struct timeval timeout;
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_LINUXSERIALPORTSERVERCONNECTOR_H_ */
