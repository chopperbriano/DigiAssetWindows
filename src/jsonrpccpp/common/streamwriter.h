// streamwriter.h - Declares StreamWriter, a small utility in the
// libjson-rpc-cpp framework that writes a full message from a std::string to a
// file descriptor, handling partial writes. Write-side counterpart to
// StreamReader; used by the RPC socket transports shared by the node and pool
// server.
#ifndef STREAMWRITER_H
#define STREAMWRITER_H

#include <memory>
#include <string>

namespace jsonrpc {
  // Writes a complete message to a file descriptor, looping over partial writes.
  class StreamWriter {
  public:
    // Write all of source to fd; returns true when fully written, false on error.
    bool Write(const std::string &source, int fd);
  };

} // namespace jsonrpc

#endif // STREAMWRITER_H
