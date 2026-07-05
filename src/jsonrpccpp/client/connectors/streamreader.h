/*************************************************************************
 * streamreader.h
 *************************************************************************
 * Placeholder StreamReader declaration living under the JSON-RPC client
 * connectors. This is an empty stub class (default-constructible, no
 * behaviour) and is distinct from jsonrpc::StreamReader in
 * common/streamreader.h, which is the real socket-reading helper used by
 * the connectors. Present only to satisfy build/link expectations in this
 * Windows fork; it plays no active role at node or pool runtime.
 ************************************************************************/

#ifndef STREAMREADER_H
#define STREAMREADER_H

// Empty placeholder type; carries no state and performs no I/O.
class StreamReader {
public:
  StreamReader();
};

#endif // STREAMREADER_H