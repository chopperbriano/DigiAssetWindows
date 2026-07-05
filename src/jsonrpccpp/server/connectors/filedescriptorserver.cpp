/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    filedescriptorserver.h
 * @date    25.10.2016
 * @author  Jean-Daniel Michaud <jean.daniel.michaud@gmail.com>
 * @license See attached LICENSE.txt
 *
 * Implementation of FileDescriptorServer: a JSON-RPC server connector that
 * reads requests from one POSIX file descriptor and writes responses to
 * another (e.g. a pipe or socket already opened by the caller). Requests are
 * newline-delimited; each is dispatched to the AbstractThreadedServer machinery.
 * This is a POSIX-only transport (uses <sys/select.h>, fcntl, unistd) and is
 * not built on the Windows node/pool.
 ************************************************************************/

#include "filedescriptorserver.h"
#include "../../common/sharedconstants.h"

#include <fcntl.h>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace jsonrpc;
using namespace std;

#define BUFFER_SIZE 1024
#ifndef DELIMITER_CHAR
#define DELIMITER_CHAR char(0x0A)
#endif
#define READ_TIMEOUT 0.001 // Set timeout in seconds

// Construct the connector over an already-open input and output fd. Passes 0
// to AbstractThreadedServer so connections are handled inline on the listener
// thread rather than on a worker pool.
FileDescriptorServer::FileDescriptorServer(int inputfd, int outputfd)
    : AbstractThreadedServer(0), inputfd(inputfd), outputfd(outputfd), reader(DEFAULT_BUFFER_SIZE) {}

// Validate that the input fd is readable and the output fd is writable before
// the listener thread starts. Returns false to abort startup if either fails.
bool FileDescriptorServer::InitializeListener() {
  if (!IsReadable(inputfd) || !IsWritable(outputfd))
    return false;
  return true;
}

// Non-blocking poll for pending input on inputfd using select() with a short
// (READ_TIMEOUT) timeout. Returns >0 when data is ready to read, 0 on timeout,
// or <0 on error; the return is passed on to HandleConnection() by the base loop.
int FileDescriptorServer::CheckForConnection() {
  FD_ZERO(&read_fds);
  FD_ZERO(&write_fds);
  FD_ZERO(&except_fds);
  FD_SET(inputfd, &read_fds);
  timeout.tv_sec = 0;
  timeout.tv_usec = (suseconds_t)(READ_TIMEOUT * 1000000);
  // Wait for something to read
  return select(inputfd + 1, &read_fds, &write_fds, &except_fds, &timeout);
}

// Read one delimiter-terminated request from inputfd, process it through the
// registered JSON-RPC handler, append the delimiter to the response, and write
// it back to outputfd. The connection handle is unused for this transport.
void FileDescriptorServer::HandleConnection(int connection) {
  (void)(connection);
  string request, response;
  reader.Read(request, inputfd, DEFAULT_DELIMITER_CHAR);
  this->ProcessRequest(request, response);
  response.append(1, DEFAULT_DELIMITER_CHAR);
  writer.Write(response, outputfd);
}

// Report whether fd was opened in a mode permitting reads (O_RDONLY/O_RDWR),
// queried via fcntl(F_GETFL). Returns false on fcntl error.
bool FileDescriptorServer::IsReadable(int fd) {
  int o_accmode = 0;
  int ret = fcntl(fd, F_GETFL, &o_accmode);
  if (ret == -1)
    return false;
  return ((o_accmode & O_ACCMODE) == O_RDONLY || (o_accmode & O_ACCMODE) == O_RDWR);
}

// Report whether fd was opened in a mode permitting writes (O_WRONLY/O_RDWR),
// queried via fcntl(F_GETFL). Returns false on fcntl error.
bool FileDescriptorServer::IsWritable(int fd) {
  int ret = fcntl(fd, F_GETFL);
  if (ret == -1)
    return false;
  return ((ret & O_WRONLY) || (ret & O_RDWR));
}
