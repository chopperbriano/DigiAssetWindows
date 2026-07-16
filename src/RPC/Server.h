//
// Created by mctrivia on 11/09/23.
//
// Server.h - JSON-RPC over HTTP server for the node (RPC::Server class).
//
// Listens on the configured rpcassetport, authenticates each request with HTTP
// Basic Auth against the config's rpcuser/rpcpassword, and dispatches JSON-RPC
// calls. A method is either served by an in-process custom handler (see
// RPC/MethodList.h) or, if unknown, forwarded to the backing DigiByte Core
// wallet via sendcommand. Responses for block-stable commands are cached. Uses
// its own boost::asio io_context and thread pool to accept and service
// connections concurrently. This is the RPC surface the node exposes to
// clients/dashboards; the pool server has its own separate HTTP layer.
//
// Constants below are the HTTP status and JSON-RPC error codes returned to
// callers.

#ifndef DIGIASSET_CORE_RPC_SERVER_H
#define DIGIASSET_CORE_RPC_SERVER_H


#define HTTP_BAD_REQUEST 400
#define HTTP_UNAUTHORIZED 401
#define RPC_METHOD_NOT_FOUND (-32601)
#define RPC_INVALID_PARAMS (-32602)
#define RPC_PARSE_ERROR (-32700)
#define RPC_FORBIDDEN_BY_SAFE_MODE (-2)
#define RPC_MISC_ERROR (-1)

// Macro definition in a common header or the RPC server file
#define REGISTER_RPC_METHOD(methodName) registerMethod(#methodName, &std::methodName)



// Specific sub-headers, not <boost/asio.hpp>, because the latter is shadowed
// by the no-op stub at src/boost/asio.hpp on this fork's include path.
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <jsonrpccpp/server.h>
#include <string>
// #include <jsonrpccpp/server/connectors/httpserver.h>
#include "ChainAnalyzer.h"
#include "DigiByteCore.h"
#include "UniqueTaskQueue.h"
#include <sstream>

using namespace std;
using namespace jsonrpc;
using boost::asio::ip::tcp;

namespace RPC {

    // JSON-RPC/HTTP server. Construction reads config, opens the listening
    // socket and spins up the io_context worker pool; start() runs the blocking
    // accept loop. Custom methods and DigiByte Core pass-through are dispatched
    // via executeCall().
    class Server {
        std::atomic<uint64_t> _callCounter{0};

        boost::asio::io_context _io{};
        // Work guard keeps _io::run() from returning when the queue is empty.
        // MUST be a member so it outlives construction — previously it was a
        // local in the ctor, which meant the thread pool ran for a few
        // microseconds and then exited the moment the ctor returned, leaving
        // posted work with nothing to execute it.
        // (Upstream mctrivia/development also moved to executor_work_guard here,
        // naming it _work; we keep the name _workGuard used throughout our ctor.)
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _workGuard;
        std::vector<std::thread> _thread_pool;

        tcp::acceptor _acceptor;
        std::string _username;
        std::string _password;
        unsigned int _port;
        std::map<std::string, bool> _allowedRPC;
        int8_t _allowRPCDefault = -1; //unknown
        bool _showParamsOnError = false;

        //functions to handle requests
        Value parseRequest(tcp::socket& socket);                                                        // read HTTP request off socket, auth-check, parse JSON body to a Value
        [[noreturn]] void accept();                                                                     // blocking accept loop: post each connection to the io_context pool
        void handleConnection(std::shared_ptr<tcp::socket> socket, uint64_t callNumber);                // service one connection end-to-end (parse, dispatch, reply, close)
        Value handleRpcRequest(const Value& request);                                                   // pull method/params/id from a request and route to executeCall
        static Value createErrorResponse(int code, const std::string& message, const Value& request);   // build a JSON-RPC error response object
        static void sendResponse(tcp::socket& socket, const Value& response);                           // serialize response and write HTTP 200 back to the socket
        bool basicAuth(const std::string& header);                                                      // validate HTTP Basic Auth header against configured user/pass
        static std::string getHeader(const std::string& headers, const std::string& wantedHeader);      // case-insensitively extract a named header value from raw headers
        void run_thread();                                                                              // io_context worker-thread body (just calls _io.run())

    public:
        explicit Server(const std::string& fileName = "config.cfg");    // load config, open listening socket, start worker pool
        ~Server();

        void start();                                                   // run the blocking accept loop until the socket closes
        unsigned int getPort();
        bool isRPCAllowed(const string& method);                        // check method against the rpcallow list (with "*" wildcard default)
        Value executeCall(const std::string& methodName, const Json::Value& params, const Json::Value& id = 1);  // dispatch one call (cache/custom handler/Core pass-through) and return its JSON result
    };

} // namespace RPC
#endif //DIGIASSET_CORE_RPC_SERVER_H
