// streamreader.h - Declares StreamReader, a small utility in the
// libjson-rpc-cpp framework that reads a delimiter-terminated message from a
// file descriptor into a std::string. Used by the RPC socket transports shared
// by the node and pool server. Owns a heap-allocated read buffer.
#ifndef STREAMREADER_H
#define STREAMREADER_H

#include <memory>
#include <string>

namespace jsonrpc {
  // Reads delimited messages from a file descriptor using a fixed-size buffer.
  class StreamReader {
  public:
    // Allocate the internal read buffer of buffersize bytes.
    StreamReader(size_t buffersize);
    virtual ~StreamReader();

    // Read from fd into target until delimiter is seen; returns true on
    // success (delimiter stripped), false on read error. See the .cpp for the
    // per-chunk delimiter-search caveat.
    bool Read(std::string &target, int fd, char delimiter);

  private:
    size_t buffersize;
    char *buffer;
  };
} // namespace jsonrpc
#endif // STREAMREADER_H
