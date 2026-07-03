//
// PoolServer - minimal HTTP server implementing the mctrivia pool wire
// protocol. One accept loop, a small thread pool, and a router that
// dispatches by (method, path) to handlers that talk to PoolDatabase.
//
// Uses boost::asio for sockets (same stack as the main exe's RPC::Server
// after win.31, so we know it works against real boost) and a minimal
// hand-rolled HTTP parser — we only need GET and POST with a handful of
// paths, so pulling in Boost.Beast would be overkill.
//

#ifndef DIGIASSET_POOL_SERVER_H
#define DIGIASSET_POOL_SERVER_H

// Specific sub-headers, NOT <boost/asio.hpp>, because src/boost/asio.hpp
// in this repo is a historical no-op stub. See
// memory/project_boost_stub_trap.md for the full story.
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PoolDatabase;

class PoolServer {
public:
    PoolServer(PoolDatabase& db, unsigned int port);
    ~PoolServer();

    PoolServer(const PoolServer&) = delete;
    PoolServer& operator=(const PoolServer&) = delete;

    // Launch the accept loop on a background thread. Returns immediately.
    void start();

    // Stop the accept loop. Best-effort; the accept socket is closed and
    // the io_context is stopped.
    void stop();

    // Live counters for the dashboard.
    unsigned int getPort() const { return _port; }
    uint64_t getRequestCount() const { return _requestCount.load(); }

    // Operator config: whether THIS pool is actually willing to distribute
    // DGB payouts right now. Default false = Phase 1/2 (registration works,
    // no money flows). When Phase 3 ships and the operator has funded and
    // verified the payout path, flip to true via `poolpayouts=1` in
    // pool.cfg. The client-side dashboard reads this via the /list response
    // body and shows "registered (no payouts yet)" instead of "active" when
    // payouts are disabled, so users aren't misled by a green status.
    void setPayoutsEnabled(bool enabled) { _payoutsEnabled.store(enabled); }
    bool getPayoutsEnabled() const { return _payoutsEnabled.load(); }

    // Info needed to serve GET /pool/stats.json (the public donation/balance
    // page). The donation address is published on the web page; the RPC creds
    // let the server read the wallet balance + amount received. All optional —
    // if unset, the stats endpoint reports zeros and no address.
    void setWalletInfo(const std::string& donationAddress,
                       const std::string& rpcUser,
                       const std::string& rpcPass,
                       int rpcPort,
                       const std::string& explorerTxPrefix,
                       const std::string& addrApiPrefix);

private:
    PoolDatabase& _db;
    unsigned int _port;

    boost::asio::io_context _io{};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _workGuard;
    boost::asio::ip::tcp::acceptor _acceptor;
    std::vector<std::thread> _threadPool;
    std::thread _acceptThread;
    std::atomic<bool> _running{false};
    std::atomic<uint64_t> _requestCount{0};
    std::atomic<bool> _payoutsEnabled{false};

    // Donation/stats state. Guarded by _statsMutex; the balance is cached and
    // only refreshed via RPC at most every ~30s so a public endpoint can't be
    // used to hammer DigiByte Core.
    std::string _donationAddress;
    std::string _rpcUser;
    std::string _rpcPass;
    int _rpcPort = 14022;
    std::string _explorerTxPrefix;
    // The treasury/donation address lives in an external wallet, so its balance
    // and received total come from a public Esplora-style explorer API
    // (GET <addrApiPrefix><address>), not the local pool wallet's RPC. The
    // local wallet's getbalance still gives "available to pay" (what payouts
    // draw from) — the operator funds it by sweeping from the treasury.
    std::string _addrApiPrefix;
    std::mutex _statsMutex;
    double _cachedAvailable = 0.0;        // local pool wallet, ready to pay
    double _cachedReceived = 0.0;         // treasury all-time received (explorer)
    double _cachedTreasuryBalance = 0.0;  // treasury current balance (explorer)
    int64_t _statsCacheTime = 0;
    // Node geolocation for the world map. _geoCache maps a node IP to a JSON
    // fragment ("lat":..,"lon":..,"city":..,"country":..); _cachedNodesJson is
    // the assembled [{...}] array rebuilt on the stats refresh cycle.
    std::map<std::string, std::string> _geoCache;
    std::string _cachedNodesJson = "[]";

    void acceptLoop();
    void handleConnection(boost::asio::ip::tcp::socket socket, uint64_t id);

    // HTTP handlers. Each returns (statusCode, contentType, body) via
    // output parameters.
    void handleRequest(const std::string& method,
                       const std::string& path,
                       const std::string& body,
                       int& outStatus,
                       std::string& outContentType,
                       std::string& outBody);

    void handlePermanent(const std::string& path, int& outStatus, std::string& outBody);
    void handleKeepalive(const std::string& body, std::string& outBody);
    void handleList(const std::string& path, const std::string& body, int& outStatus, std::string& outBody);
    void handleNodes(std::string& outBody);
    void handleMap(std::string& outBody);
    void handleBad(std::string& outBody);
    void handleStats(std::string& outBody);
};

#endif // DIGIASSET_POOL_SERVER_H
