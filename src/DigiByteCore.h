//
// Created by mctrivia on 30/01/23.
// Krzysztof Okupski's libbitcoin-api-cpp for reference
//
// DigiByteCore.h - C++ wrapper around a DigiByte Core full node's JSON-RPC
// interface. This is how the node (and pool server) talk to the underlying
// digibyted daemon: it owns the HTTP JSON-RPC client, serializes calls under a
// shared mutex, and exposes one typed method per RPC (getblock,
// getrawtransaction, wallet, mining, raw-tx, etc.) returning the structs from
// DigiByteCore_Types.h. Connection settings come from a config file.
//
// Key node-specific additions over the reference library:
//  - getBlockVerbose(): fetches a block with verbosity 2 and pre-loads every
//    transaction into an in-memory TX cache, so the block processor can pull
//    each TX via getRawTransaction() without an extra RPC round-trip.
//  - the capitalized wrapper methods (getBlockCount, getBlockHash, getBlock,
//    getRawTransaction, listUnspent, getAddressInfo) route through
//    errorCheckAPI() to convert raw JSON-RPC faults into the friendlier nested
//    exception types.
//  - _useAssetPort selects an alternate RPC port (rpcassetport) when set.
//

#ifndef DIGIBYTECORE_CONFIGDIGIBYTECORE_H
#define DIGIBYTECORE_CONFIGDIGIBYTECORE_H


#include "DigiByteCore_Exception.h"
#include "DigiByteCore_Types.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <iomanip>
#include <jsonrpccpp/client.h>
#include <jsonrpccpp/client/connectors/httpclient.h>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#if defined(_WIN32)
using uint = unsigned int;

#if defined(GetMessage)
#undef GetMessage
#endif // GetMessage

#if defined(max)
#undef max
#endif // max

#if defined(min)
#undef min
#endif // min

#endif // _WIN32

namespace jsonrpc {
    class HttpClient;

    class Client;
} // namespace jsonrpc

/**
 * Typed C++ facade over a DigiByte Core node's JSON-RPC API. Holds the HTTP
 * client/connection, serializes every call through a static mutex, and offers
 * one method per RPC returning the structs in DigiByteCore_Types.h. Also keeps
 * a per-block transaction cache and simple call-count/timing profiling. Not
 * connected until makeConnection() succeeds; methods requiring a connection
 * throw exceptionDigiByteCoreNotConnected otherwise.
 */
class DigiByteCore {
public:
    // Address format requested from getnewaddress.
    enum AddressTypes {
        LEGACY,
        SEGWIT,
        BECH32
    };

    // Detected DigiByte Core wallet/RPC version, inferred from the shape of
    // scriptPubKey in getrawtransaction results (see coreVersion()).
    enum WalletVersion {
        unknown= 0,
        v7=7,         //7 or less
        v8=8          //8 or higher
    };

    // A fully self-contained pre-fetched block: the header/tx-id list PLUS this
    // block's own transaction data (never the shared _txCache). Consumed by the
    // ChainAnalyzer sync loop. `error` carries any fetch failure to the consumer
    // (re-thrown there); `endOfChain` marks that the producer reached the tip.
    struct PrefetchedBlock {
        blockinfo_t info;
        std::map<std::string, getrawtransaction_t> txData;
        std::exception_ptr error = nullptr;
        bool endOfChain = false;
    };

private:
    std::unique_ptr<jsonrpc::HttpClient> httpClient = nullptr;
    std::unique_ptr<jsonrpc::Client> client = nullptr;
    uint64_t _dgbToSat(std::string value);
    static std::mutex& getLock(); //never destroyed - safe to use during process exit
    bool _useAssetPort = false;


    std::string _configFileName = "config.cfg";

    // Wraps an RPC lambda: throws if not connected, and translates raw
    // DigiByteException failures into the class's own exception types.
    template<typename fn_t>
    auto errorCheckAPI(fn_t fn) -> decltype(fn());

    long long _runTime = 0;      // cumulative RPC time (microseconds) for profiling
    unsigned int _runCount = 0;  // number of RPC calls made, for profiling
    WalletVersion _walletVersion = unknown;

    // TX cache for prefetched data (loaded before processing a block)
    std::mutex _txCacheMutex;
    std::map<std::string, getrawtransaction_t> _txCache;

    // --- Block prefetch pipeline (opt-in; used only during deep bulk sync) ---
    // A single producer thread walks the chain from a start hash, fetching each
    // block (verbosity 2) into a SELF-CONTAINED PrefetchedBlock - its own txData
    // map, NOT the shared _txCache - so an in-flight prefetch can never clobber
    // the block the consumer is still draining (the original crash). A bounded
    // queue caps memory + read-ahead. Errors are captured into the block and
    // re-thrown by the consumer, so an RPC failure surfaces instead of hanging.
    std::thread _pfThread;
    std::mutex _pfMutex;
    std::condition_variable _pfSpaceCv; // producer waits when queue full
    std::condition_variable _pfDataCv;  // consumer waits when queue empty
    std::deque<PrefetchedBlock> _pfQueue;
    bool _pfStop = false;
    static const size_t _pfMaxDepth = 4; // bounded read-ahead
    void prefetchLoop(std::string hash);


public:
    // Returns the detected Core wallet version, forcing a probe RPC if not yet
    // known (see DigiByteCore.cpp).
    WalletVersion coreVersion();


    // Returns a formatted one-line row of accumulated RPC profiling stats:
    // total time, average time per call, and call count.
    std::string printProfilingInfo() {
        long long totalDuration = _runTime;
        int transactions = _runCount;
        long long avgDuration = transactions > 0 ? totalDuration / transactions : 0;

        std::ostringstream oss;
        oss << std::right << std::setw(30) << "DigiByte Core"
            << std::setw(20) << totalDuration
            << std::setw(20) << avgDuration
            << std::setw(20) << transactions << std::endl;
        return oss.str();
    }

    //constructor/destructor
    DigiByteCore() = default;
    ~DigiByteCore();

    //functions that drop connection
    void setFileName(const std::string& fileName, bool useAssetPort = false);
    void setConfig(const std::string& username, const std::string& password, const std::string& address = "127.0.0.1",
                   uint port = 14022);
    void dropConnection();

    //functions that create connection
    void makeConnection(); //will throw an error if we can't connect

    //overrides the http timeout set from config.cfg's rpctimeout(ms).  Useful for individual
    //calls that are known to legitimately run long(eg issueasset's funding retries)
    void setTimeout(unsigned int milliseconds);

    //config based getter
    std::string getFileName();

    //functions that require a connection.  all will throw a not connected error if connection lost
    uint getBlockCount();
    std::string getBlockHash(uint height);
    blockinfo_t getBlock(const std::string& hash);
    getrawtransaction_t getRawTransaction(const std::string& txid);
    blockinfo_t getBlockVerbose(const std::string& hash); // getblock with verbosity 2 — loads TX cache

    // --- Block prefetch pipeline (opt-in via config pipelinesync) ---
    // startPrefetch: launch the producer thread walking the chain from startHash.
    // getNextPrefetchedBlock: pop the next block in order (blocks until one is
    //   ready or the producer stops); returns false only if stopped with nothing.
    // loadPrefetchedTxCache: install a consumed block's txData as the current
    //   _txCache so getRawTransaction() serves this block's TXs.
    // stopPrefetch: signal + join the producer and clear the queue (idempotent).
    void startPrefetch(const std::string& startHash);
    bool getNextPrefetchedBlock(PrefetchedBlock& out);
    void loadPrefetchedTxCache(std::map<std::string, getrawtransaction_t>&& txData);
    void stopPrefetch();
    std::vector<unspenttxout_t> listUnspent(int minconf = 1, int maxconf = 99999999, const std::vector<std::string>& addresses = {});
    getaddressinfo_t getAddressInfo(const std::string& address);


    /* === Auxiliary functions === */
    Json::Value sendcommand(const std::string& command, const Json::Value& params);

    std::string IntegerToString(int num);
    std::string RoundDouble(double num);


    /* === Node functions === */
    void addnode(const std::string& node, const std::string& command);
    std::vector<nodeinfo_t> getaddednodeinfo(bool dns);
    std::vector<nodeinfo_t> getaddednodeinfo(bool dns, const std::string& node);
    std::vector<peerinfo_t> getpeerinfo();
    int getconnectioncount();


    /* === Wallet functions === */
    void backupwallet(const std::string& destination);
    std::string encryptwallet(const std::string& passphrase);
    void walletlock();
    void walletpassphrase(const std::string& passphrase, int timeout);
    void walletpassphrasechange(const std::string& oldpassphrase, const std::string& newpassphrase);

    std::string dumpprivkey(const std::string& digibyteaddress);
    void importprivkey(const std::string& digibyteprivkey);
    void importprivkey(const std::string& digibyteprivkey, const std::string& label, bool rescan = true);
    void importaddress(const std::string& address, const std::string& account, bool rescan = true);

    std::string addmultisigaddress(int nrequired, const std::vector<std::string>& keys);
    std::string addmultisigaddress(int nrequired, const std::vector<std::string>& keys, const std::string& account);
    multisig_t createmultisig(int nrequired, const std::vector<std::string>& keys);
    std::string getnewaddress(const std::string& label = "", AddressTypes type = BECH32);
    validateaddress_t validateaddress(const std::string& digibyteaddress);

    void keypoolrefill();
    bool settxfee(double amount);
    double estimatefee(int blocks);

    std::string signmessage(const std::string& digibyteaddress, const std::string& message);
    bool verifymessage(const std::string& digibyteaddress, const std::string& signature, const std::string& message);

    getinfo_t getinfo();
    void stop();


    /* === Accounting === */
    double getbalance();
    double getbalance(const std::string& account, int minconf = 1, bool includewatchonly = false);
    double getunconfirmedbalance();

    double getreceivedbyaccount(const std::string& account, int minconf = 1);
    double getreceivedbyaddress(const std::string& digibyteaddress, int minconf = 1);

    std::vector<accountinfo_t> listreceivedbyaccount(int minconf = 1, bool includeempty = false);
    std::vector<addressinfo_t> listreceivedbyaddress(int minconf = 1, bool includeempty = false);

    gettransaction_t gettransaction(const std::string& tx, bool watch);
    std::vector<transactioninfo_t> listtransactions();
    std::vector<transactioninfo_t> listtransactions(const std::string& account, int count = 10, int from = 0);


    std::vector<std::string> getaddressesbylabel(const std::string& label, const std::string& type = "");
    std::vector<std::string> listlabels(const std::string& purpose = "");
    std::vector<std::vector<addressgrouping_t>> listaddressgroupings();

    std::string sendtoaddress(const std::string& digibyteaddress, double amount);
    std::string sendtoaddress(const std::string& digibyteaddress, double amount, const std::string& comment,
                              const std::string& comment_to);

    std::string sendfrom(const std::string& fromaccount, const std::string& todigibyteaddress, double amount);
    std::string sendfrom(const std::string& fromaccount, const std::string& todigibyteaddress, double amount,
                         const std::string& comment, const std::string& comment_to, int minconf = 1);

    std::string sendmany(const std::string& fromaccount, const std::map<std::string, double>& amounts);
    std::string
    sendmany(const std::string& fromaccount, const std::map<std::string, double>& amounts, const std::string comment,
             int minconf = 1);

    utxoinfo_t gettxout(const std::string& txid, int n, bool includemempool = true);
    utxosetinfo_t gettxoutsetinfo();

    std::vector<unspenttxout_t> listunspent(int minconf = 1, int maxconf = 99999999, const std::vector<std::string>& addresses = {});
    std::vector<txout_t> listlockunspent();
    bool lockunspent(bool unlock, const std::vector<txout_t>& outputs);


    /* === Mining functions === */
    std::string getbestblockhash();
    std::string getblockhash(int blocknumber);
    blockinfo_t getblock(const std::string& blockhash);
    int getblockcount();

    void setgenerate(bool generate, int genproclimit = -1);
    bool getgenerate();
    double getdifficulty();
    int gethashespersec();

    mininginfo_t getmininginfo();
    workdata_t getwork();
    bool getwork(const std::string& data);

    txsinceblock_t listsinceblock(const std::string& blockhash = "", int target_confirmations = 1);


    /* === Low level calls === */
    getrawtransaction_t getrawtransaction(const std::string& txid, bool verbose = false);
    decodescript_t decodescript(const std::string& hexString);
    decoderawtransaction_t decoderawtransaction(const std::string& hexString);
    std::string sendrawtransaction(const std::string& hexString, bool highFee);

    std::string createrawtransaction(const std::vector<txout_t>& inputs, const std::map<std::string, double>& amounts);
    std::string
    createrawtransaction(const std::vector<txout_t>& inputs, const std::map<std::string, std::string>& amounts);

    signrawtransaction_t signrawtransaction(const std::string& rawTx,
                                            const std::vector<signrawtxin_t> inputs = std::vector<signrawtxin_t>());
    signrawtransaction_t signrawtransaction(const std::string& rawTx, const std::vector<signrawtxin_t> inputs,
                                            const std::vector<std::string>& privkeys,
                                            const std::string& sighashtype = "ALL");

    std::vector<std::string> getrawmempool();
    std::string getrawchangeaddress();


    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */
    // Base exception for this wrapper; what() prefixes the message with
    // "DigiByte Core Exception: ".
    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "DigiByte Core Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    // Thrown when the node was reachable before but a call now fails to reach
    // (or is refused by) DigiByte Core.
    class exceptionCoreOffline : public exception {
    public:
        explicit exceptionCoreOffline()
            : exception("Core Offline") {}
    };

    // Thrown when a connection-requiring call runs before makeConnection().
    class exceptionDigiByteCoreNotConnected : public exception {
    public:
        explicit exceptionDigiByteCoreNotConnected()
            : exception("Core not connected") {} //Run makeConnection()
    };
};


#endif //DIGIBYTECORE_CONFIGDIGIBYTECORE_H
