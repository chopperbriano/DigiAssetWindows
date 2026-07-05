/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    iprocedureinvokationhandler.h
 * @date    23.10.2014
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: part of the bundled libjson-rpc-cpp server
// library that backs the node's and pool server's JSON-RPC endpoints. This
// header declares the abstract callback interface that a protocol handler uses
// to dispatch a decoded, validated request to the actual RPC method
// implementation. It decouples the wire-protocol layer (parsing/validating
// JSON-RPC v1/v2 envelopes) from the code that runs the named procedure.

#ifndef JSONRPC_CPP_IPROCEDUREINVOKATIONHANDLER_H
#define JSONRPC_CPP_IPROCEDUREINVOKATIONHANDLER_H

namespace Json {
  class Value;
}

namespace jsonrpc {

  class Procedure;

  // Abstract sink implemented by whatever object owns the RPC method bodies
  // (e.g. the server stub). The protocol handlers call into these two methods
  // once a request has been parsed and validated.
  class IProcedureInvokationHandler {
  public:
    virtual ~IProcedureInvokationHandler() {}
    // Invoke a method-style call (has an id, expects a reply). Runs the
    // procedure identified by proc using the params in input and writes the
    // procedure's return value into output.
    virtual void HandleMethodCall(Procedure &proc, const Json::Value &input, Json::Value &output) = 0;
    // Invoke a notification-style call (no id, no reply). Runs proc with the
    // params in input; any result is discarded.
    virtual void HandleNotificationCall(Procedure &proc, const Json::Value &input) = 0;
  };
} // namespace jsonrpc

#endif // JSONRPC_CPP_IPROCEDUREINVOKATIONHANDLER_H
