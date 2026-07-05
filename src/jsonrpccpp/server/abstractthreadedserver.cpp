/*************************************************************************
 * abstractthreadedserver.cpp
 *************************************************************************
 * Implements AbstractThreadedServer: the threaded server-connector base used
 * by the DigiAsset node/pool RPC transports. It runs a dedicated background
 * listener thread that polls for incoming connections and dispatches each one
 * to a worker thread pool (or handles it inline when the pool size is zero).
 * Transport-specific accept/read/write is provided by subclass overrides of
 * InitializeListener/CheckForConnection/HandleConnection.
 ************************************************************************/

#include "abstractthreadedserver.h"

using namespace jsonrpc;
using namespace std;

// Construct with a worker pool of the given size; start out not running.
AbstractThreadedServer::AbstractThreadedServer(size_t threads) : running(false), threadPool(threads), threads(threads) {}

AbstractThreadedServer::~AbstractThreadedServer() { this->StopListening(); }

// Initialize the transport listener and, on success, mark the server running
// and spawn the background thread that runs ListenLoop. Returns false if
// already running or if InitializeListener fails.
bool AbstractThreadedServer::StartListening() {
  if (this->running)
    return false;

  if (!this->InitializeListener())
    return false;

  this->running = true;

  this->listenerThread = unique_ptr<thread>(new thread(&AbstractThreadedServer::ListenLoop, this));

  return true;
}

// Clear the running flag so ListenLoop exits, then join the listener thread.
// Returns false if it was not running.
bool AbstractThreadedServer::StopListening() {
  if (!this->running)
    return false;

  this->running = false;

  this->listenerThread->join();
  return true;
}

// Listener-thread body: while running, poll for a new connection. When one
// arrives, enqueue it on the thread pool (or handle it inline if threads == 0);
// otherwise sleep briefly to avoid busy-spinning.
void AbstractThreadedServer::ListenLoop() {
  while (this->running) {
    int conn = this->CheckForConnection();

    if (conn > 0) {
      if (this->threads > 0) {
        this->threadPool.enqueue(&AbstractThreadedServer::HandleConnection, this, conn);
      } else {
        this->HandleConnection(conn);
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}
