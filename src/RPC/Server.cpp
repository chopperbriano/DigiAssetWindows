//
// Created by mctrivia on 11/09/23.
//
// Server.cpp - Implementation of the node's JSON-RPC/HTTP server (RPC::Server).
//
// Accepts HTTP connections on the configured RPC port, verifies HTTP Basic
// Auth, parses the JSON-RPC request, and dispatches it: cached block-stable
// results are returned immediately, in-process custom methods are invoked from
// the `methods` table, and anything else is forwarded to DigiByte Core. This
// fork deliberately includes individual boost::asio sub-headers to dodge the
// no-op <boost/asio.hpp> stub on the include path (see notes below and in the
// header). See RPC/Server.h for the class overview and error-code constants.

#include "RPC/Server.h"
#include "AppMain.h"
#include "Config.h"
#include "DigiByteCore.h"
#include "Log.h"
#include "RPC/MethodList.h"
#include "Version.h"
#include "utils.h"
#include <algorithm>
// Include specific boost::asio sub-headers rather than <boost/asio.hpp>.
// src/boost/asio.hpp is a no-op stub on this fork's include path — including
// the top-level header gets the stub, which silently breaks all socket ops.
// Including individual sub-headers bypasses the stub because the stub only
// shadows <boost/asio.hpp>, not <boost/asio/*.hpp>. Also need the real boost
// include path added via compile_flags in CMakeLists.txt.
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <iostream>
#include <sstream>
#include <jsonrpccpp/client.h>
#include <jsonrpccpp/client/connectors/httpclient.h>
#include <memory>
#include <thread>
#include <vector>



using namespace std;

namespace RPC {
    namespace {
        ///List of RPC commands whose values will not change until a new block is added
        vector<string> cacheableRpcCommands = {
                "getbestblockhash",
                "getblock",
                "getblockchaininfo",
                "getblockcount",
                "getblockfilter",
                "getblockhash",
                "getblockheader",
                "getblockstats",
                "getchaintips",
                "getchaintxstats",
                "getdifficulty",
                "getmempoolancestors",
                "getmempooldescendants",
                "getmempoolentry",
                "getmempoolinfo",
                "getrawmempool",
                "gettxout",
                "gettxoutproof",
                "gettxoutsetinfo",
                "preciousblock",
                "pruneblockchain",
                "savemempool",
                "scantxoutset",
                "verifychain",
                "verifytxoutproof"};

        // Simple Base64 decoder for Basic Auth when OpenSSL is unavailable
        static std::string base64Decode(const std::string& input) {
            static const std::string charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string decoded;
            int val = 0;
            int valb = -8;
            for (unsigned char c : input) {
                if (isspace(c) || c == '=') {
                    if (c == '=') break;
                    continue;
                }
                size_t pos = charset.find(c);
                if (pos == std::string::npos) break;
                val = (val << 6) + static_cast<int>(pos);
                valb += 6;
                if (valb >= 0) {
                    decoded.push_back(char((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
            return decoded;
        }
    } // namespace

    /**
     * Small class that allows easy forcing close socket when it goes out of scope
     */
    class SocketRAII {
    public:
        explicit SocketRAII(tcp::socket& s) : socket(s) {}
        ~SocketRAII() { socket.close(); }

    private:
        tcp::socket& socket;
    };

    /*
    ███████╗███████╗████████╗██╗   ██╗██████╗
    ██╔════╝██╔════╝╚══██╔══╝██║   ██║██╔══██╗
    ███████╗█████╗     ██║   ██║   ██║██████╔╝
    ╚════██║██╔══╝     ██║   ██║   ██║██╔═══╝
    ███████║███████╗   ██║   ╚██████╔╝██║
    ╚══════╝╚══════╝   ╚═╝    ╚═════╝ ╚═╝
     */
    /**
     * Builds the RPC server from a config file. Reads rpcuser/rpcpassword,
     * rpcassetport (default 14024), rpcthreads (default 16), the rpcallow map
     * and rpcdebugshowparamsonerror; opens, binds and listens on the TCP
     * acceptor, then spins up the worker thread pool running _io.run(). Throws
     * (and logs CRITICAL) if any step fails. Does NOT begin accepting
     * connections — call start() for that. See the inline notes re:
     * _workGuard/_acceptor being members to keep the io_context alive and avoid
     * the boost::asio stub.
     * @param fileName path to the config file (default "config.cfg")
     */
    Server::Server(const string& fileName)
        : _io(),
          _workGuard(boost::asio::make_work_guard(_io)),
          _acceptor(_io) {
        // _workGuard is a MEMBER now, not a local. Previously this was:
        //     auto work = boost::asio::make_work_guard(_io);
        // inside the ctor body, which meant the work guard was destroyed the
        // moment the ctor returned. With no work guard, _io.run() on every
        // pool thread returned immediately because the queue was empty, the
        // thread pool emptied, and any boost::asio::post() from accept() had
        // no worker threads to run it. Making it a member keeps _io alive
        // for the full lifetime of Server and keeps the pool actually running.
        //
        // _acceptor is also in the init list because real boost::asio's
        // basic_socket_acceptor has no default constructor — the previous
        // stub header at src/boost/asio.hpp did have one (that no-op'd every
        // operation), which is exactly why RPC never actually listened.
        Log* log = Log::GetInstance();

        try {
            Config config = Config(fileName);
            _username = config.getString("rpcuser");
            _password = config.getString("rpcpassword");
            _port = config.getInteger("rpcassetport", 14024);

            // Bind LOOPBACK by default (the documented localhost-only design).
            // This RPC forwards wallet money-movement calls to Core, so it must
            // not be LAN/internet-reachable. rpcbind can override; a bad value
            // falls back to 127.0.0.1. (audit MUST-FIX #7)
            std::string bindAddr = config.getString("rpcbind", "127.0.0.1");
            boost::system::error_code bindEc;
            boost::asio::ip::address bindIp = boost::asio::ip::make_address(bindAddr, bindEc);
            if (bindEc) bindIp = boost::asio::ip::make_address("127.0.0.1");
            tcp::endpoint endpoint(bindIp, _port);
            log->addMessage("RPC::Server ctor: opening acceptor on port " + std::to_string(_port), Log::INFO);
            _acceptor.open(endpoint.protocol());
            log->addMessage("RPC::Server ctor: open() returned", Log::INFO);
            _acceptor.set_option(tcp::acceptor::reuse_address(true));
            log->addMessage("RPC::Server ctor: set_option() returned", Log::INFO);
            _acceptor.bind(endpoint);
            log->addMessage("RPC::Server ctor: bind() returned", Log::INFO);
            _acceptor.listen();
            log->addMessage("RPC::Server ctor: listen() returned", Log::INFO);

            // Ask boost what it thinks the acceptor is bound to. If listen
            // succeeded but the endpoint is bogus or the native socket is
            // invalid, this will disagree with our expectations.
            try {
                auto le = _acceptor.local_endpoint();
                std::ostringstream oss;
                oss << "RPC::Server ctor: local_endpoint = " << le.address().to_string()
                    << ":" << le.port() << " is_open=" << _acceptor.is_open()
                    << " native=" << (long long)_acceptor.native_handle();
                log->addMessage(oss.str(), Log::INFO);
            } catch (const std::exception& e) {
                log->addMessage(std::string("RPC::Server ctor: local_endpoint() threw: ") + e.what(), Log::CRITICAL);
            }

            _allowedRPC = config.getBoolMap("rpcallow");
            _showParamsOnError = config.getBool("rpcdebugshowparamsonerror", false);

            // Create a pool of threads to run all of the io_services.
            // Adopt upstream (mctrivia/development) ordering: bind/listen the
            // acceptor BEFORE spawning the pool, so a bind failure throws
            // without leaking worker threads. We keep our config key/default
            // (rpcthreads, 16); upstream uses rpcparallel/8.
            size_t poolSize = config.getInteger("rpcthreads", 16);
            for (std::size_t i = 0; i < poolSize; ++i) {
                _thread_pool.emplace_back([this] { run_thread(); });
            }

            log->addMessage("RPC Server listening on port " + std::to_string(_port));
        } catch (const std::exception& e) {
            log->addMessage(std::string("RPC::Server ctor exception: ") + e.what(), Log::CRITICAL);
            throw;
        } catch (...) {
            log->addMessage("RPC::Server ctor unknown exception", Log::CRITICAL);
            throw;
        }
    }

    /**
     * Destructor. Joins every worker thread in the pool so the io_context's
     * threads are fully torn down before the object is destroyed.
     */
    Server::~Server() {
        // Wait for all threads in the pool to exit.
        for (auto& thread: _thread_pool) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    /**
     * Worker-thread body: runs the io_context event loop. Blocks in _io.run()
     * processing posted connection-handling work until the io_context is
     * stopped or runs out of work (kept alive by _workGuard).
     */
    void Server::run_thread() {
        _io.run();
    }

    /**
     * Enters the blocking accept loop (accept()). Returns only when the loop
     * exits — i.e. the listening socket has died and the server can no longer
     * receive requests. On exit it logs CRITICAL and clears AppMain's RPC
     * server pointer so the dashboard reports RPC as down rather than showing a
     * stale "up" from a dangling pointer.
     */
    void Server::start() {
        Log* log = Log::GetInstance();
        try {
            accept();
        } catch (const std::exception& e) {
            log->addMessage(std::string("RPC server stopped: ") + e.what(), Log::CRITICAL);
        } catch (...) {
            log->addMessage("RPC server stopped with unknown error", Log::CRITICAL);
        }
        // start() returning means accept() exited, which means the whole
        // accept loop is dead and this server will never receive another
        // request. Clear the AppMain pointer so the dashboard can report
        // honestly that RPC is down, instead of showing a stale green
        // "Port 14024" from a dangling raw pointer that used to be alive.
        log->addMessage("RPC Server accept loop exited (socket closed)", Log::CRITICAL);
        AppMain::GetInstance()->setRpcServer(nullptr);
    }

    /*
     ██████╗ ███████╗███╗   ██╗███████╗██████╗ ██╗ ██████╗
    ██╔════╝ ██╔════╝████╗  ██║██╔════╝██╔══██╗██║██╔════╝
    ██║  ███╗█████╗  ██╔██╗ ██║█████╗  ██████╔╝██║██║
    ██║   ██║██╔══╝  ██║╚██╗██║██╔══╝  ██╔══██╗██║██║
    ╚██████╔╝███████╗██║ ╚████║███████╗██║  ██║██║╚██████╗
     ╚═════╝ ╚══════╝╚═╝  ╚═══╝╚══════╝╚═╝  ╚═╝╚═╝ ╚═════╝
     */

    /**
     * Infinite accept loop. For each incoming connection it assigns a
     * monotonically increasing call number, blocks in _acceptor.accept(), then
     * posts handleConnection() onto the io_context so a pool thread services it
     * while this loop goes back to accepting. Per-connection accept errors are
     * caught and logged at DEBUG so one bad connection never breaks the loop.
     * Marked [[noreturn]] — it only leaves by throwing (propagated to start()).
     */
    void Server::accept() {
        Log* log = Log::GetInstance();
        while (true) {
            uint64_t callNumber = _callCounter++;
            try {
                auto socket = std::make_shared<tcp::socket>(_io);
                _acceptor.accept(*socket);
                log->addMessage("RPC call #" + std::to_string(callNumber) + " received", Log::DEBUG);
                boost::asio::post(_io, [this, socket, callNumber]() { this->handleConnection(socket, callNumber); });
            } catch (const std::exception& e) {
                log->addMessage("RPC accept error: " + std::string(e.what()), Log::DEBUG);
            } catch (...) {
                log->addMessage("RPC accept unknown error", Log::DEBUG);
            }
        }
    }

    /**
     * Services a single accepted connection end-to-end on a pool thread: parses
     * the request, dispatches it via handleRpcRequest(), and writes back either
     * the result or a JSON-RPC error response, then closes the socket (via
     * SocketRAII). Silently drops liveness-probe/empty connections (the "empty"
     * DigiByteException) without replying. Optionally logs request params on
     * error when rpcdebugshowparamsonerror is set, and always logs the call's
     * duration in microseconds at DEBUG.
     * @param socket shared socket for this connection (closed on return)
     * @param callNumber sequence id used only for correlating log lines
     */
    void Server::handleConnection(std::shared_ptr<tcp::socket> socket, uint64_t callNumber) {
        Log* log = Log::GetInstance();

        std::chrono::steady_clock::time_point startTime;
        string method = "NA";
        try {
            //get the socket
            SocketRAII socketGuard(*socket); //make sure socket always gets closed

            //start timer and add debug log
            log->addMessage("RPC call #" + std::to_string(callNumber) + " started", Log::DEBUG);
            startTime = std::chrono::steady_clock::now();

            // Handle the request and send the response
            Value response;
            Value request;
            bool error = false;
            try {
                request = parseRequest(*socket);
                method = request["method"].asString();
                log->addMessage("RPC call #" + std::to_string(callNumber) + " method: " + method, Log::DEBUG);
                response = handleRpcRequest(request);
            } catch (const DigiByteException& e) {
                // Stub / probe connections: DigiByteException's ctor wraps the
                // raw message in "Error during parsing of >>...<<" because it
                // tries to JSON-parse the message, so match on the wrapped
                // form. Dashboard's RPC liveness probe (TCP connect + close)
                // produces this on every check.
                const std::string& em = e.getMessage();
                if (em == "empty" || em.find(">>empty<<") != std::string::npos) return;
                log->addMessage("Expected exception in RPC call #" + std::to_string(callNumber) + ": " + em, Log::DEBUG);
                response = createErrorResponse(e.getCode(), em, request);
                error = true;
            } catch (const std::exception& e) {
                log->addMessage("Unexpected exception in RPC call #" + std::to_string(callNumber) + ": " + e.what(), Log::DEBUG);
                response = createErrorResponse(RPC_MISC_ERROR, "Unexpected Error", request);
                error = true;
            } catch (...) {
                log->addMessage("Unknown exception in RPC call #" + std::to_string(callNumber), Log::DEBUG);
                response = createErrorResponse(RPC_MISC_ERROR, "Unexpected Error", request);
                error = true;
            }

            sendResponse(*socket, response);
            if (error && _showParamsOnError && request.isMember("params")) {
                log->addMessage("RPC call #" + std::to_string(callNumber) + " params: " + request["params"].toStyledString(), Log::DEBUG);
            }
        } catch (const std::exception& e) {
            log->addMessage("Unexpected exception trying to reply to RPC call #" + std::to_string(callNumber) + ": " + e.what(), Log::DEBUG);
        } catch (...) {
            log->addMessage("Unknown exception trying to reply to RPC call #" + std::to_string(callNumber), Log::DEBUG);
        }


        //calculate time taken
        auto duration = std::chrono::steady_clock::now() - startTime;
        log->addMessage("RPC call #" + std::to_string(callNumber) + " finished in " + to_string(std::chrono::duration_cast<std::chrono::microseconds>(duration).count()) + " us", Log::DEBUG);
    }

    /**
     * Reads a full HTTP request off the socket and returns its parsed JSON body.
     * Reads the first chunk, splits headers from body at the blank line, rejects
     * empty reads ("empty") and header-less requests, enforces Basic Auth via
     * basicAuth(), then reads the remainder of the body up to Content-Length and
     * parses it as JSON. Throws DigiByteException on empty/malformed request,
     * failed auth (HTTP_UNAUTHORIZED) or JSON parse error (RPC_PARSE_ERROR).
     * @param socket connected client socket to read from
     * @return the parsed JSON-RPC request document
     */
    Value Server::parseRequest(tcp::socket& socket) {
        // Bound how long any single read may block, so a slowloris client that
        // opens a socket and dribbles bytes (or none) can't park this worker
        // thread forever. On timeout read_some throws and the connection is
        // dropped by the caller's catch. (audit low)
#ifdef _WIN32
        DWORD rcvTimeout = 10000; // ms
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));
#else
        struct timeval rcvTimeout; rcvTimeout.tv_sec = 10; rcvTimeout.tv_usec = 0;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const void*>(&rcvTimeout), sizeof(rcvTimeout));
#endif

        // Read the HTTP request headers
        char data[1024];
        size_t length = socket.read_some(boost::asio::buffer(data, sizeof(data)));

        // Empty read — no client connected (stub accept or closed connection)
        if (length == 0) {
            throw DigiByteException(HTTP_BAD_REQUEST, "empty");
        }

        // Parse the HTTP request headers to determine content length
        string requestStr(data, length);
        size_t bodyStart = requestStr.find("\r\n\r\n");
        if (bodyStart == string::npos) {
            throw DigiByteException(HTTP_BAD_REQUEST, "Invalid HTTP request: No request body found.");
        }
        string headers = requestStr.substr(0, bodyStart);

        //verify the authentication
        if (!basicAuth(headers)) {
            throw DigiByteException(HTTP_UNAUTHORIZED, "Invalid HTTP request: No valid authentication provided.");
        }

        // Find the Content-Length header. A missing/garbage value would make
        // stoi throw; a huge value would make the read loop below exhaust
        // memory. Validate and cap it. (audit low)
        int contentLength = 0;
        try {
            contentLength = stoi(getHeader(headers, "Content-Length"));
        } catch (...) {
            throw DigiByteException(HTTP_BAD_REQUEST, "Invalid or missing Content-Length");
        }
        const int MAX_BODY = 16 * 1024 * 1024; // 16 MB is far beyond any real RPC call
        if (contentLength < 0 || contentLength > MAX_BODY) {
            throw DigiByteException(HTTP_BAD_REQUEST, "Request body length out of range");
        }

        // Prepare to read the body
        std::string jsonContent = requestStr.substr(bodyStart + 4);
        int remainingContent = contentLength - jsonContent.size();

        // Read the rest of the body if not all content was received
        while (remainingContent > 0) {
            char buffer[1024];
            std::size_t bytesRead = socket.read_some(boost::asio::buffer(buffer, std::min(remainingContent, static_cast<int>(sizeof(buffer)))));
            jsonContent.append(buffer, bytesRead);
            remainingContent -= bytesRead;
        }

        // Parse the JSON content
        Json::CharReaderBuilder readerBuilder;
        Json::Value doc;
        istringstream jsonContentStream(jsonContent);
        string errs;

        if (!Json::parseFromStream(readerBuilder, jsonContentStream, &doc, &errs)) {
            throw DigiByteException(RPC_PARSE_ERROR, "JSON parsing error: " + errs);
        }

        return doc;
    }

    /**
     * Extracts a single HTTP header's value from the raw header block. Matching
     * is case-insensitive on the header name, but the returned value is sliced
     * from the original (case-preserved) text. Throws DigiByteException
     * (HTTP_BAD_REQUEST) if the header is absent.
     * @param headers raw header block (everything before the blank line)
     * @param wantedHeader header name to look up, e.g. "Content-Length"
     * @return the header's value with no surrounding "Name: " or CRLF
     */
    string Server::getHeader(const string& headers, const string& wantedHeader) {
        //find the auth header
        string lowerHeaders = headers;
        string lowerWantedHeader = wantedHeader;
        transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(), ::tolower);
        transform(lowerWantedHeader.begin(), lowerWantedHeader.end(), lowerWantedHeader.begin(), ::tolower);

        size_t start = lowerHeaders.find(lowerWantedHeader + ": ");
        if (start == string::npos) throw DigiByteException(HTTP_BAD_REQUEST, wantedHeader + " header not found");
        size_t end = lowerHeaders.find("\r\n", start);
        size_t headerLength = lowerWantedHeader.length() + 2; //+2 for ": "
        return headers.substr(start + headerLength, end - start - headerLength);
    }

    /**
     * Validates HTTP Basic Auth. Pulls the Authorization header, strips the
     * "Basic " prefix, base64-decodes the credentials and compares them against
     * the configured "user:password".
     * @param headers raw HTTP header block
     * @return true if the supplied credentials match the configured ones
     */
    // Basic authentication function
    bool Server::basicAuth(const string& headers) {
        string authHeader = getHeader(headers, "Authorization");

        // Extract and decode the base64-encoded credentials
        string base64Credentials = authHeader.substr(6); // Remove "Basic "
        string decoded = base64Decode(base64Credentials);

        return (decoded == _username + ":" + _password);
    }

    /**
     * Core dispatch for one RPC call. Rejects methods not permitted by the
     * rpcallow list (throws RPC_FORBIDDEN_BY_SAFE_MODE). Returns a cached
     * response if one is still valid. Otherwise invokes the in-process handler
     * from the `methods` table, or falls back to forwarding the call to DigiByte
     * Core via sendcommand — marking non-block-stable Core commands (those not
     * in cacheableRpcCommands) as non-cacheable. The resulting response is added
     * to the cache and returned as JSON.
     * @param methodName RPC method name
     * @param params JSON params array (or null)
     * @param id JSON-RPC id echoed back in the response (default 1)
     * @return the JSON-RPC result/response for this call
     */
    Value Server::executeCall(const std::string& methodName, const Json::Value& params, const Json::Value& id) {
        AppMain* app = AppMain::GetInstance();

        //see if method on approved list
        if (!isRPCAllowed(methodName)) throw DigiByteException(RPC_FORBIDDEN_BY_SAFE_MODE, methodName + " is forbidden");

        //see if cached
        Response response;
        if (app->getRpcCache()->isCached(methodName, params, response)) return response.toJSON(id);

        //see if custom method handler
        if (methods.find(methodName) != methods.end()) {
            // Method exists in the map, so call it and set the result
            response = methods[methodName](params);
        } else {
            // Method does not exist in the map, fallback to sending to DigiByte core
            Json::Value walletResponse = app->getDigiByteCore()->sendcommand(methodName, params);
            response.setResult(walletResponse);

            //if the method is not it cacheableRpcCommands then disable caching
            if (std::find(cacheableRpcCommands.begin(), cacheableRpcCommands.end(), methodName) == cacheableRpcCommands.end()) {
                response.setBlocksGoodFor(-1);
            }
        }

        //cache response
        app->getRpcCache()->addResponse(methodName, params, response);

        //return as json
        return response.toJSON(id);
    }

    /**
     * Validates and unpacks a parsed JSON-RPC request, then dispatches it.
     * Extracts the optional "id" (null if absent), requires a string "method"
     * (else RPC_METHOD_NOT_FOUND), and requires "params" — if present — to be an
     * array (else RPC_INVALID_PARAMS). Forwards to executeCall().
     * @param request parsed JSON-RPC request object
     * @return the JSON-RPC response for the call
     */
    Value Server::handleRpcRequest(const Value& request) {
        Json::Value id;

        //lets get the id(user defined value they can use as a reference)
        if (request.isMember("id")) {
            id = request["id"];
        } else {
            id = Value(Json::nullValue);
        }

        //lets get the command the user wants
        if (!request.isMember("method") || !request["method"].isString()) {
            throw DigiByteException(RPC_METHOD_NOT_FOUND, "Method not found");
        }
        string methodName = request["method"].asString();

        //lets check params is present
        if (request.isMember("params") && !request["params"].isArray()) {
            throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
        }
        const Json::Value& params = request.isMember("params") ? request["params"] : Value(Json::nullValue);

        return executeCall(methodName, params, id);
    }


    /**
     * Builds a JSON-RPC error response object with null result and an
     * {code, message} error member, echoing the request's id when present.
     * @param code JSON-RPC error code
     * @param message human-readable error text
     * @param request originating request (used only to echo its id)
     * @return the assembled error response object
     */
    Value Server::createErrorResponse(int code, const string& message, const Value& request) {
        // Create a JSON-RPC error response
        Json::Value response;
        response["result"] = Json::nullValue;
        Json::Value error;
        error["code"] = code;
        error["message"] = message;
        response["error"] = error;
        if (request.isMember("id")) response["id"] = request["id"];
        return response;
    }

    /**
     * Serializes a JSON response to compact (un-indented) text, wraps it in a
     * minimal HTTP/1.1 200 OK message with Content-Length and JSON content type,
     * and writes it to the socket.
     * @param socket connected client socket to write to
     * @param response JSON value to send as the body
     */
    void Server::sendResponse(tcp::socket& socket, const Value& response) {
        Json::StreamWriterBuilder writer;
        writer.settings_["indentation"] = "";

        // Serialize JSON object to string.
        string jsonResponse = writeString(writer, response);

        // Create the HTTP response string.
        string httpResponse = "HTTP/1.1 200 OK\r\n";
        httpResponse += "Content-Length: " + to_string(jsonResponse.length()) + "\r\n";
        httpResponse += "Content-Type: application/json\r\n\r\n";
        httpResponse += jsonResponse;

        // Write the HTTP response to the socket.
        boost::asio::write(socket, boost::asio::buffer(httpResponse.c_str(), httpResponse.length()));
    }


    /*
     ██████╗ ███████╗████████╗████████╗███████╗██████╗ ███████╗
    ██╔════╝ ██╔════╝╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗██╔════╝
    ██║  ███╗█████╗     ██║      ██║   █████╗  ██████╔╝███████╗
    ██║   ██║██╔══╝     ██║      ██║   ██╔══╝  ██╔══██╗╚════██║
    ╚██████╔╝███████╗   ██║      ██║   ███████╗██║  ██║███████║
     ╚═════╝ ╚══════╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝╚══════╝
     */
    unsigned int Server::getPort() {
        return _port;
    }

    /**
     * Decides whether a method may be served, per the config's rpcallow map. An
     * explicit entry wins; otherwise the cached wildcard default is used, which
     * is computed once (lazily) from the "*" entry. The temporary self-recursion
     * guard (_allowRPCDefault set to 0 before the "*" lookup) prevents an
     * infinite loop while resolving the wildcard.
     * @param method RPC method name
     * @return true if the method is allowed
     */
    bool Server::isRPCAllowed(const string& method) {
        auto it = _allowedRPC.find(method);
        if (it == _allowedRPC.end()) {
            if (_allowRPCDefault == -1) {
                _allowRPCDefault = 0; //default.  must set to prevent possible infinite loop
                _allowRPCDefault = isRPCAllowed("*") ? 1 : 0;
            }
            return _allowRPCDefault;
        }
        return it->second;
    }

} // namespace RPC