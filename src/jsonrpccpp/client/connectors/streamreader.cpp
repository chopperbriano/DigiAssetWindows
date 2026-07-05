/*************************************************************************
 * streamreader.cpp
 *************************************************************************
 * Implementation of the placeholder StreamReader stub (see streamreader.h).
 * The constructor is a no-op; the class holds no state and does no I/O.
 * The real stream-reading logic used by the JSON-RPC socket connectors
 * lives in common/streamreader.h / .cpp (jsonrpc::StreamReader).
 ************************************************************************/

#include "streamreader.h"

// No-op constructor; nothing to initialise.
StreamReader::StreamReader() {}
