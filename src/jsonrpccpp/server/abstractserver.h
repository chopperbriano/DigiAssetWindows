/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    abstractserver.h
 * @date    30.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

// Role in DigiAsset for Windows: AbstractServer<S> is the CRTP base that a
// concrete RPC server class derives from to expose its C++ methods over
// JSON-RPC. It wires a transport (AbstractServerConnector) to a
// version-specific protocol handler, keeps the tables mapping procedure names
// to member-function pointers, and invokes the right member when a request
// arrives. Used to build the node's/pool's RPC endpoint.

#ifndef JSONRPC_CPP_ABSTRACTSERVER_H_
#define JSONRPC_CPP_ABSTRACTSERVER_H_

#include "abstractserverconnector.h"
#include "iclientconnectionhandler.h"
#include "iprocedureinvokationhandler.h"
#include "requesthandlerfactory.h"
#include <jsonrpccpp/common/procedure.h>
#include <map>
#include <string>
#include <vector>

namespace jsonrpc {

  // CRTP server base parameterized on the concrete server type S. Member
  // pointers into S are stored per procedure name and invoked on request.
  template <class S>
  class AbstractServer : public IProcedureInvokationHandler {
  public:
    // Member-function pointer types for a request-reply method and a fire-and-
    // forget notification handler on the concrete server S.
    typedef void (S::*methodPointer_t)(const Json::Value &parameter, Json::Value &result);
    typedef void (S::*notificationPointer_t)(const Json::Value &parameter);

    // Bind a transport connector to a protocol handler for the requested
    // JSON-RPC version (default v2) and register this server as the invocation
    // target so incoming requests are routed to the bound member functions.
    AbstractServer(AbstractServerConnector &connector, serverVersion_t type = JSONRPC_SERVER_V2) : connection(connector) {
      this->handler = RequestHandlerFactory::createProtocolHandler(type, *this);
      connector.SetHandler(this->handler);
    }

    virtual ~AbstractServer() { delete this->handler; }

    bool StartListening() { return connection.StartListening(); }

    bool StopListening() { return connection.StopListening(); }

    // Invoke the member function bound to proc's name, passing the request
    // params and filling output with the result. Called by the protocol
    // handler for RPC_METHOD requests.
    virtual void HandleMethodCall(Procedure &proc, const Json::Value &input, Json::Value &output) {
      S *instance = dynamic_cast<S *>(this);
      (instance->*methods[proc.GetProcedureName()])(input, output);
    }

    // Invoke the notification member bound to proc's name (no result). Called
    // by the protocol handler for RPC_NOTIFICATION requests.
    virtual void HandleNotificationCall(Procedure &proc, const Json::Value &input) {
      S *instance = dynamic_cast<S *>(this);
      (instance->*notifications[proc.GetProcedureName()])(input);
    }

  protected:
    // Register proc as an RPC method and bind pointer as its handler. Returns
    // false (no-op) if proc is not a method or the name is already taken.
    bool bindAndAddMethod(const Procedure &proc, methodPointer_t pointer) {
      if (proc.GetProcedureType() == RPC_METHOD && !this->symbolExists(proc.GetProcedureName())) {
        this->handler->AddProcedure(proc);
        this->methods[proc.GetProcedureName()] = pointer;
        return true;
      }
      return false;
    }

    // Register proc as an RPC notification and bind pointer as its handler.
    // Returns false (no-op) if proc is not a notification or the name is taken.
    bool bindAndAddNotification(const Procedure &proc, notificationPointer_t pointer) {
      if (proc.GetProcedureType() == RPC_NOTIFICATION && !this->symbolExists(proc.GetProcedureName())) {
        this->handler->AddProcedure(proc);
        this->notifications[proc.GetProcedureName()] = pointer;
        return true;
      }
      return false;
    }

  private:
    AbstractServerConnector &connection;
    IProtocolHandler *handler;
    std::map<std::string, methodPointer_t> methods;
    std::map<std::string, notificationPointer_t> notifications;

    // True if name is already bound as either a method or a notification,
    // used to prevent duplicate procedure registration.
    bool symbolExists(const std::string &name) {
      if (methods.find(name) != methods.end())
        return true;
      if (notifications.find(name) != notifications.end())
        return true;
      return false;
    }
  };

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_ABSTRACTSERVER_H_ */
