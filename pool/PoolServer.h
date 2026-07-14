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
#include <set>
#include <string>
#include <thread>
#include <vector>

class PoolDatabase;

// The pool exe's HTTP front door. Owns the listen socket and worker
// threads, and routes each request to a handler that reads/writes the
// shared PoolDatabase. Lifetime: constructed once in pool/main.cpp, bound
// in the ctor, then start()ed after the first-run snapshot; stop()ped and
// deleted on shutdown. All public getters/setters are thread-safe (atomics
// or _statsMutex-guarded) since the dashboard thread reads them live.
class PoolServer {
public:
    // Bind the listen socket on `port` (throws on bind failure) and hold a
    // reference to the shared `db`. Does NOT begin accepting — call start().
    PoolServer(PoolDatabase& db, unsigned int port);
    // Stops the server (best-effort) and joins threads.
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

    // Shared secret gating POST /permanent/add (the marketplace/operator
    // ingestion endpoint). Set once from pool.cfg `pooladmintoken` BEFORE
    // start(); an empty token disables the endpoint entirely (403). Because
    // it's assigned before the accept threads launch and only read afterwards,
    // a plain string is safe (no lock needed).
    void setIngestToken(const std::string& token) { _ingestToken = token; }

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

    // Peer pools (independent pools that are AWARE of each other). `peers` is a
    // list of peer base URLs (e.g. https://pool-b.digistamp.co); `token` is a
    // shared secret both sides present on /peer/* calls. Set once before start().
    // When non-empty, start() launches a background sync that polls each peer's
    // /peer/status (liveness + stats), mirrors its permanent list, and caches its
    // nodes for the merged world map. Empty peers = feature off.
    void setPeers(const std::vector<std::string>& peers, const std::string& token);

    // Snapshot of what we last learned from each peer (thread-safe copy). The
    // dashboard + payout dedup read this.
    struct PeerState {
        std::string url;
        bool up = false;
        int64_t lastSeen = 0;
        std::string version;
        unsigned int nodesActive = 0;
        double treasuryBalance = 0.0;
        double paidTotal = 0.0;
    };
    std::vector<PeerState> getPeerStates();

    // Recent payouts a peer reports (address -> most-recent paidAt within the
    // window), for cross-pool payout dedup. Queries the peer live; returns false
    // if the peer is unreachable (caller then pays without dedup - availability
    // over strict coordination). windowSeconds bounds "recent".
    bool fetchPeerPaidAddresses(const std::string& peerUrl, int64_t windowSeconds,
                                std::map<std::string, int64_t>& outAddrToPaidAt);
    std::vector<std::string> getPeerUrls() const { return _peers; }
    std::string getPeerToken() const { return _peerToken; }

    // ---- Discovery (seed + gossip -> DISPLAY-ONLY directory) ----
    // Open auto-discovery: this pool announces its own public URL to a seed and
    // gossips GET /peer/list with the pools it learns, converging on a directory
    // of every pool on the network. DISPLAY-ONLY and UNTRUSTED: discovered pools
    // show up on the map/network view but are NEVER used for list-mirroring or
    // payout-dedup - that stays gated to the explicit poolpeers + token. Set
    // before start(); empty seed AND empty publicUrl = discovery off.
    //   publicUrl : this pool's OWN public base URL (so it can announce itself)
    //   seed      : bootstrap seed pool URL (ships defaulted to the flagship pool)
    void setDiscovery(const std::string& publicUrl, const std::string& seed);

    // Phase 2: on-chain discovery. When enabled, the pool ANNOUNCES its public URL
    // in a DigiByte OP_RETURN (weekly) and SCANS new blocks for other pools'
    // announcements - so pools find each other with NO seed. Still display-only
    // (announced URLs are probe-validated before listing). Announcing costs a tiny
    // tx fee + needs the pool wallet (walletPass unlocks it if encrypted); scanning
    // is read-only. Set before start(). Uses the same Core RPC as setWalletInfo().
    void setOnchain(bool enabled, const std::string& walletPass) { _onchain = enabled; _walletPass = walletPass; }

    struct DiscoveredPool {
        std::string url;
        int64_t lastSeen = 0;
        bool up = false;
        unsigned int nodesActive = 0;
        double treasuryBalance = 0.0;
    };

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
    // Shared secret for POST /permanent/add; empty = endpoint disabled.
    std::string _ingestToken;

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
    // Held with try_lock by the ONE thread doing a stats refresh, so blocking
    // network I/O (Core RPC + explorer + ip-api) happens with _statsMutex
    // RELEASED and never stalls the worker pool: other threads skip the refresh
    // and serve the last-known-good cache instead of blocking. (audit M3)
    std::mutex _statsRefreshMutex;
    double _cachedAvailable = 0.0;        // local pool wallet, ready to pay
    double _cachedReceived = 0.0;         // treasury all-time received (explorer)
    double _cachedTreasuryBalance = 0.0;  // treasury current balance (explorer)
    int64_t _statsCacheTime = 0;
    // Node geolocation for the world map. _geoCache maps a node IP to a JSON
    // fragment ("lat":..,"lon":..,"city":..,"country":..); _cachedNodesJson is
    // the assembled [{...}] array rebuilt on the stats refresh cycle.
    std::map<std::string, std::string> _geoCache;
    std::string _cachedNodesJson = "[]";

    // ---- Peer pools (awareness) ----
    std::vector<std::string> _peers;   // peer base URLs (set before start())
    std::string _peerToken;            // shared secret for /peer/* calls
    std::thread _peerThread;
    std::atomic<bool> _peerRunning{false};
    std::mutex _peerMutex;             // guards _peerStates + _peerNodesJson
    std::map<std::string, PeerState> _peerStates;   // url -> last known state
    std::map<std::string, std::string> _peerNodesJson; // url -> that peer's nodes[] (tagged), for the merged map
    // Background loop: poll each peer's /peer/status, mirror its permanent list,
    // and cache its node geo. Runs only when _peers is non-empty.
    void peerSyncLoop();
    // GET /peer/status | /peer/ledger | /peer/assets — token-gated peer API.
    void handlePeerStatus(const std::string& query, int& outStatus, std::string& outBody);
    void handlePeerLedger(const std::string& query, int& outStatus, std::string& outBody);
    void handlePeerAssets(const std::string& query, int& outStatus, std::string& outBody);
    // True if the token in the request query/header matches _peerToken (or the
    // token is unset, meaning peer API is open). Used to gate /peer/*.
    bool peerAuthOk(const std::string& query) const;

    // ---- Discovery (open, display-only) ----
    std::string _publicUrl;   // this pool's own public URL (to announce)
    std::string _seed;        // bootstrap seed URL
    std::thread _discoveryThread;
    std::atomic<bool> _discoveryRunning{false};
    std::map<std::string, DiscoveredPool> _directory;       // guarded by _peerMutex
    std::map<std::string, std::string> _directoryNodesJson; // url -> tagged nodes[] for the map
    void discoveryLoop();
    // GET /peer/list  - the pools this pool knows (open); POST /peer/announce -
    // a pool announces its URL to join the directory (open, validated).
    void handlePeerList(const std::string& query, int& outStatus, std::string& outBody);
    void handlePeerAnnounce(const std::string& body, int& outStatus, std::string& outBody);
    // Fetch <url>/pool/stats.json, confirm it's a real pool, fill a DiscoveredPool
    // (+ its tagged nodes array). Returns false if unreachable / not a pool.
    bool probePool(const std::string& url, DiscoveredPool& out, std::string& outNodesJson);

    // ---- On-chain discovery (phase 2) ----
    bool _onchain = false;
    std::string _walletPass;
    void onchainAnnounce();                        // publish our URL in an OP_RETURN (weekly)
    void onchainScan(std::set<std::string>& out);  // read new blocks for peer announcements
    std::string rpcRaw(const std::string& method, const std::string& paramsJson); // Core RPC -> raw JSON

    // Blocking accept loop (runs on _acceptThread): accepts sockets and
    // posts each to the io_context thread pool for handling.
    void acceptLoop();
    // Reads one HTTP request off `socket`, dispatches via handleRequest, and
    // writes the response. `id` is a per-connection counter for logging.
    void handleConnection(boost::asio::ip::tcp::socket socket, uint64_t id);

    // HTTP handlers. Each returns (statusCode, contentType, body) via
    // output parameters.
    void handleRequest(const std::string& method,
                       const std::string& path,
                       const std::string& body,
                       const std::string& clientIp,
                       int& outStatus,
                       std::string& outContentType,
                       std::string& outBody);

    // GET /permanent/<page>.json — serves one page of the permanent asset
    // list (assetId/txHash -> CIDs) that clients pin, mirroring mctrivia's
    // wire format. Parses the page number out of `path`.
    void handlePermanent(const std::string& path, int& outStatus, std::string& outBody);
    // POST /permanent/add — token-gated ingestion. Adds an asset's CIDs to the
    // permanent list so pool nodes pin+serve it. Body is JSON:
    //   {"token":"...","assetId":"La..","txHash":"<issuance txid>","cids":"cid1,cid2"}
    // Idempotent (INSERT OR IGNORE). Lets the marketplace publish freshly
    // minted assets so they propagate to wallets via the pool fleet.
    void handlePermanentAdd(const std::string& body, int& outStatus, std::string& outBody);
    // POST /keepalive — a registered node checking in. Records the node's
    // liveness (keyed off `body` + `clientIp`) in the pool database.
    void handleKeepalive(const std::string& body, const std::string& clientIp, std::string& outBody);
    // GET/POST /list/<floor>.json — registers a payout address and returns
    // the assets a node should pin at/above the given payout floor. Reports
    // the pool's payout-enabled state so the client shows an honest status.
    void handleList(const std::string& path, const std::string& body, const std::string& clientIp, int& outStatus, std::string& outBody);
    // GET /nodes.json — public list of registered/verified nodes.
    void handleNodes(std::string& outBody);
    // GET /map.json — geolocated node points for the landing-page world map.
    void handleMap(std::string& outBody);
    // GET /bad.json — assets/CIDs flagged as bad (skip-pin list).
    void handleBad(std::string& outBody);
    // GET /pool/stats.json — public donation/treasury page: cached wallet
    // available balance, treasury received/balance, and node geo points.
    void handleStats(std::string& outBody);
};

#endif // DIGIASSET_POOL_SERVER_H
