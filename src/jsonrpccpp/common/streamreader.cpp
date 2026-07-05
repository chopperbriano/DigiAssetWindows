// streamreader.cpp - Implementation of StreamReader, the low-level helper the
// libjson-rpc-cpp framework uses to read a delimited JSON-RPC message off a
// file descriptor (e.g. a TCP/pipe socket). It underpins the RPC transports
// used by both the node (DigiAssetWindows.exe) and the pool server. The
// constructor allocates a fixed read buffer that is freed by the destructor.
#include "streamreader.h"
#include <stdlib.h>
#include <string.h>
#include "unistd.h"

using namespace jsonrpc;
using namespace std;

StreamReader::StreamReader(size_t buffersize) : buffersize(buffersize), buffer(static_cast<char *>(malloc(buffersize))) {}

StreamReader::~StreamReader() { free(buffer); }

// Read a full delimited message from file descriptor fd into target.
// Repeatedly reads up to buffersize bytes, appending each chunk to target,
// until the delimiter byte appears in the most recent chunk. On any read()
// error (negative return) it returns false. On success it removes the trailing
// delimiter character (pop_back) and returns true. Note: the delimiter is only
// searched for within the latest buffer read, not across the whole target.
bool StreamReader::Read(std::string &target, int fd, char delimiter) {
  ssize_t bytesRead;
  do {
    bytesRead = read(fd, this->buffer, buffersize);
    if (bytesRead < 0) {
      return false;
    } else {
      target.append(buffer, static_cast<size_t>(bytesRead));
    }
  } while (memchr(buffer, delimiter, bytesRead) == NULL); //(target.find(delimiter) == string::npos && bytesRead > 0);

  target.pop_back();
  return true;
}
