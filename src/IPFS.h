//
// Created by mctrivia on 15/06/23.
//
// IPFS: the node's controller/worker for talking to a local IPFS (Kubo) node
// over its HTTP API (default http://localhost:5001/api/v0/). It runs on its own
// background thread (Threaded) that pulls queued jobs out of the Database and
// executes them: downloading (cat) DigiAsset content, and pinning/unpinning
// CIDs for the Permanent Storage Pool. Callers submit work asynchronously
// (callOnDownload with a preregistered callback, or a promise/sync variant) or
// synchronously (isPinned/getSize/downloadFile). It also provides pure helpers
// (sha256<->CID conversion, CID/URL validation, a hard-coded "known lost" CID
// skip list) and getPeerId(), which derives this node's public multiaddr so the
// pool server can dial it. Used by both the node and pool-server deployables.
//

#ifndef DIGIASSET_CORE_IPFS_H
#define DIGIASSET_CORE_IPFS_H

#include "DigiAssetRules.h"
#include "DigiByteCore_Types.h"
#include "Threaded.h"
#include <functional>
#include <future>
#include <mutex>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

/**
 * cid,extra,content,failed
 */
using IPFSCallbackFunction = std::function<void(const std::string&, const std::string&, const std::string&, bool)>;


/**
 * IPFS controller. Inherits Threaded: its worker loop (mainFunction) drains the
 * Database job queue. Configuration (node URL, timeouts, parallelism) is read
 * from the config file in the constructor.
 */
class IPFS : public Threaded {

private:
    const static std::vector<std::string> _knownLostCID;
    std::string _nodePrefix = "http://localhost:5001/api/v0/";

    ///timeout times are in seconds
    unsigned int _timeoutPin = 1200;
    unsigned int _timeoutDownload = 3600;
    unsigned int _timeoutRetry = 3600;
    unsigned int _maxParallel = 10;

    // Worker-thread body: pops one queued IPFS job (download/pin/unpin) from the
    // Database, executes it against the local node, then removes the job and
    // fires its callback. Sleeps briefly when the queue is empty.
    void mainFunction() override;
    // Returns this machine's public WAN IP by querying external IP-echo services.
    static std::string getIP();
    // Chooses the best multiaddr from the node's advertised addresses that a
    // remote peer can dial: prefers a direct TCP address containing our WAN IP,
    // then a p2p-circuit relay address, then falls back to substituting our IP
    // into a local address. Picks lowest port when several qualify.
    static std::string findPublicAddress(const std::vector<std::string>& addresses, const std::string& ip);
    // Parses the /id JSON response and returns its "Addresses" list.
    static std::vector<std::string> extractAddresses(const std::string& idString);
    // Pull the bare "ID" field from the /id JSON response (the local
    // node's own peerId, e.g. "12D3KooW..." or "Qm..."). Returns empty
    // string if the field is missing or the JSON is malformed.
    static std::string extractIdField(const std::string& idString);

    //TestHelpers
    // Low-level HTTP POST to the local node's API (url = _nodePrefix + command).
    // Returns the response body, or saves it to outputPath when given (binary
    // download). Translates Curl timeout/connection errors into IPFS exceptions.
    std::string
    _command(const std::string& command, const std::map<std::string, std::string>& data = {}, unsigned int timeout = 0, const std::string& outputPath = "") const;


public:
    // Loads IPFS settings from configFile and, unless runStart is false, starts
    // the worker thread.
    IPFS(const std::string& configFile, bool runStart = true);

    //helpers
    // Converts a 256-bit SHA256 hash into the base32 CIDv1 of raw data encoded
    // with that hash (valid only for raw-mode data up to ~2MB).
    static std::string sha256ToCID(BitIO& hash);
    // Same as above but takes the hash as a hex string.
    static std::string sha256ToCID(const std::string& hash);
    // True if url starts (case-insensitively) with "ipfs://" and the remainder
    // is a valid CID.
    static bool isIPFSurl(const std::string& url);
    // Strips the "ipfs://" prefix and returns the CID; throws if not an IPFS url.
    static std::string getCID(const std::string& url);
    // True if the CID is in the hard-coded "known lost" skip list.
    static bool isLostCID(const std::string& cid);
    // Cheap sanity check that a CID is non-empty and fully alphanumeric.
    static bool isValidCID(const std::string& cid);

    //called by initializing code
    // Registers a named callback (via the Database) so it can be referenced by
    // symbol when queuing download jobs.
    static void registerCallback(const std::string& callbackSymbol, const IPFSCallbackFunction& callback);

    //async requests
    // Queues a download and runs the preregistered callbackSymbol when done.
    // Jobs sharing a non-empty sync value run in order; if the CID is already
    // pinned and sync is empty the callback may run immediately/inline.
    void callOnDownload(const std::string& cid, const std::string& sync, const std::string& extra,
                        const std::string& callbackSymbol, unsigned int maxTime = 0);
    // Queues a download and returns a promise resolved with the content (or an
    // exception). Resolves immediately if the CID is already pinned.
    std::promise<std::string>
    callOnDownloadPromise(const std::string& cid, const std::string& sync = "", unsigned int maxTime = 0);
    // Blocking download: waits on callOnDownloadPromise's future and returns the content.
    std::string callOnDownloadSync(const std::string& cid, const std::string& sync = "", unsigned int maxTime = 0);
    // Queues a pin job. maxSize: 0 = don't pin, 1 = pin regardless of size,
    // >1 = pin only if the object is smaller than that many bytes.
    void pin(const std::string& cid, unsigned int maxSize = 1); //1 is any size
    // Queues an unpin job for the CID.
    void unpin(const std::string& cid);

    //synchronous requests
    // Blocking check of whether the local node has the CID pinned.
    bool isPinned(const std::string& cid) const;
    // Blocking query of an object's cumulative size in bytes (object/stat).
    unsigned int getSize(const std::string& cid) const;
    // Blocking download of a CID to filePath; optionally pins it first (used for
    // startup files that must be present).
    void downloadFile(const std::string& cid, const std::string& filePath, bool pinAlso = false);
    // Returns this node's dialable multiaddr (peerId) for the pool server, using
    // getIP + the /id response filtered to addresses that are actually ours.
    std::string getPeerId() const;


    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */
    // Base class for all IPFS errors; prefixes messages with "IPFS Exception: ".
    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "IPFS Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    // Thrown when a request exceeds its allotted time.
    class exceptionTimeout : public exception {
    public:
        explicit exceptionTimeout()
            : exception("Timeout") {}
    };

    // Thrown when a supplied CID fails validation.
    class exceptionInvalidCID : public exception {
    public:
        explicit exceptionInvalidCID(const std::string& cid = "")
            : exception(cid.empty() ? "Invalid CID Provided" : cid + " is not a valid CID") {}
    };

    // Thrown when the local IPFS node cannot be reached (likely down).
    class exceptionNoConnection : public exception {
    public:
        explicit exceptionNoConnection()
            : exception("IPFS Node Likely Down") {}
    };
};


#endif //DIGIASSET_CORE_IPFS_H
