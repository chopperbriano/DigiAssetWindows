/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    linuxserialportserver.cpp
 * @date    01.11.2019
 * @author  Billy Araujo
 * @license See attached LICENSE.txt
 *
 * Implementation of LinuxSerialPortServer: a JSON-RPC server connector that
 * reads requests from and writes responses to a Linux serial port device
 * (e.g. /dev/ttyS0), opened O_RDWR. Requests/responses are newline-delimited
 * and dispatched via the AbstractThreadedServer machinery. Linux-only
 * transport; not built on the Windows node/pool.
 ************************************************************************/

#include "linuxserialportserver.h"
#include "../../common/sharedconstants.h"
#include "../../common/streamreader.h"
#include "../../common/streamwriter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <iostream>
#include <sstream>
#include <string>

using namespace jsonrpc;
using namespace std;

#define BUFFER_SIZE 1024
#ifndef DELIMITER_CHAR
#define DELIMITER_CHAR char(0x0A)
#endif

#define READ_TIMEOUT 0.001 // Set timeout in seconds

// Construct the connector for a named serial device with a worker pool of the
// given size. The device is not opened until InitializeListener() runs.
LinuxSerialPortServer::LinuxSerialPortServer(const std::string &deviceName, size_t threads)
    : AbstractThreadedServer(threads), deviceName(deviceName), reader(DEFAULT_BUFFER_SIZE) {}

// Close the serial file descriptor on destruction.
LinuxSerialPortServer::~LinuxSerialPortServer() { close(this->serial_fd); }

// Open the serial device O_RDWR, storing the fd in serial_fd.
// @return true if the open succeeded (fd >= 0), false otherwise.
bool LinuxSerialPortServer::InitializeListener() {

  serial_fd = open(deviceName.c_str(), O_RDWR);

  return serial_fd >= 0;
}

// Non-blocking poll for pending input on the serial fd using select() with a
// short (READ_TIMEOUT) timeout. Returns >0 when data is ready, 0 on timeout,
// <0 on error; the value drives the base ListenLoop's dispatch decision.
int LinuxSerialPortServer::CheckForConnection() {
  FD_SET(serial_fd, &read_fds);
  timeout.tv_sec = 0;
  timeout.tv_usec = (suseconds_t)(READ_TIMEOUT * 1000000);
  // Wait for something to read
  return select(serial_fd + 1, &read_fds, nullptr, nullptr, &timeout);
}

// Read one delimiter-terminated request from the serial fd, process it through
// the registered JSON-RPC handler, append the delimiter, and write the response
// back to the same fd. The connection handle is unused for this transport.
void LinuxSerialPortServer::HandleConnection(int connection) {
  (void)(connection);
  string request, response;
  reader.Read(request, serial_fd, DEFAULT_DELIMITER_CHAR);
  this->ProcessRequest(request, response);
  response.append(1, DEFAULT_DELIMITER_CHAR);
  writer.Write(response, serial_fd);
}
