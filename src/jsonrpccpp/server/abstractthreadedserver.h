/*************************************************************************
 * abstractthreadedserver.h
 *************************************************************************
 * Base class for JSON-RPC server connectors that accept connections on a
 * dedicated listener thread and dispatch each accepted connection to a
 * ThreadPool of worker threads. Concrete connectors (e.g. the file-descriptor
 * and serial-port servers) supply the transport-specific accept/read/write
 * logic by overriding the pure-virtual hooks below. Part of the bundled
 * libjson-rpc-cpp used by the DigiAsset node/pool to expose their RPC APIs.
 ************************************************************************/

#ifndef ABSTRACTTHREADEDSERVER_H
#define ABSTRACTTHREADEDSERVER_H

#include "abstractserverconnector.h"
#include "threadpool.h"
#include <memory>
#include <thread>

namespace jsonrpc {
  /**
   * @brief Threaded server-connector base class. Runs a background listener
   * loop that polls for incoming connections and hands each off to a thread
   * pool for request processing. Subclasses implement the transport details.
   */
  class AbstractThreadedServer : public AbstractServerConnector {
  public:
    /**
     * @brief Construct the server with a worker pool of the given size.
     * @param threads number of worker threads used to process connections
     */
    AbstractThreadedServer(size_t threads);
    virtual ~AbstractThreadedServer();

    /**
     * @brief Initialize the listener and spawn the background listener thread.
     * @return false if already running or InitializeListener() fails, else true
     */
    virtual bool StartListening();

    /**
     * @brief Clear the running flag and join the listener thread.
     * @return false if it was not running, true once stopped and joined
     */
    virtual bool StopListening();

  protected:
    /**
     * @brief InitializeListener should initialize sockets, file descriptors etc.
     * @return
     */
    virtual bool InitializeListener() = 0;

    /**
     * @brief CheckForConnection should poll for a new connection. This must be
     * a non-blocking call.
     * @return a handle which is passed on to HandleConnection()
     */
    virtual int CheckForConnection() = 0;

    /**
     * @brief HandleConnection must handle connection information for a given
     * handle that has been returned by CheckForConnection()
     * @param connection
     */
    virtual void HandleConnection(int connection) = 0;

  private:
    bool running;
    std::unique_ptr<std::thread> listenerThread;
    ThreadPool threadPool;
    size_t threads;

    /**
     * @brief Listener-thread body. Repeatedly calls CheckForConnection() while
     * running; when a connection is available it is dispatched to the thread
     * pool (or handled inline when the pool size is zero) via HandleConnection().
     */
    void ListenLoop();
  };
} // namespace jsonrpc

#endif // ABSTRACTTHREADEDSERVER_H
