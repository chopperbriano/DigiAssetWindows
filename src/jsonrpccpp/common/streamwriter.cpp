// streamwriter.cpp - Implementation of StreamWriter, the low-level helper the
// libjson-rpc-cpp framework uses to write a complete JSON-RPC message to a file
// descriptor (e.g. a TCP/pipe socket), the write-side counterpart to
// StreamReader. Used by the RPC transports shared by the node
// (DigiAssetWindows.exe) and the pool server.
#include "streamwriter.h"
#include "unistd.h"

using namespace jsonrpc;
using namespace std;

// Write the entire contents of source to file descriptor fd.
// Loops issuing write() calls until every byte has been sent, advancing an
// offset by the number of bytes written each iteration to handle partial
// writes. Returns false on any write() error (negative return), true once all
// bytes are written.
bool StreamWriter::Write(const string &source, int fd) {
  ssize_t bytesWritten;
  size_t totalSize = source.size();
  size_t remainingSize = totalSize;

  do {
    bytesWritten = write(fd, source.c_str() + (totalSize - remainingSize), remainingSize);
    if (bytesWritten < 0) {
      return false;
    } else {
      remainingSize -= static_cast<size_t>(bytesWritten);
    }
  } while (remainingSize > 0);
  return true;
}
