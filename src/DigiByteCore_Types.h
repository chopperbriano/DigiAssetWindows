/**
 * @file    types.h
 * @author  Krzysztof Okupski
 * @date    29.10.2014
 * @version 1.0
 *
 * Type definitions for the JSON-RPC C++ interface.
 *
 * DigiByteCore_Types.h - Plain structs that mirror the JSON results returned by
 * DigiByte Core's RPC calls. The DigiByteCore wrapper (DigiByteCore.h/.cpp)
 * parses each RPC's JSON response into the matching struct here, so callers work
 * with typed C++ fields instead of raw Json::Value. Field names follow the RPC
 * response keys 1:1. Grouped by area: general info, node/peer, account/address,
 * transactions, UTXO/other, and some as-yet-unused block/mining types.
 */


#ifndef DIGIASSET_CORE_DIGIBYTECORE_TYPES_H
#define DIGIASSET_CORE_DIGIBYTECORE_TYPES_H


#include <string>
#include <vector>

#include <jsoncpp/json/json.h>

/* === General types === */
/* Result of the getinfo RPC: node version, balance, block height, connections,
 * difficulty, wallet/keypool state, and any error string. */
struct getinfo_t {
    int version;
    int protocolversion;
    int walletversion;
    double balance;
    int blocks;
    int timeoffset;
    int connections;
    std::string proxy;
    double difficulty;
    bool testnet;
    int keypoololdest;
    int keypoolsize;
    double paytxfee;
    int unlocked_until;
    std::string errors;
};


/* === Node types === */
/* One resolved address for an added node, with its connection direction. */
struct netaddress_t {
    std::string address;
    std::string connected;
};

/* One entry from getaddednodeinfo: the added node string, whether it is
 * connected, and its resolved addresses. */
struct nodeinfo_t {
    std::string addednode;
    bool connected;
    std::vector<netaddress_t> addresses;
};

/* One connected peer from getpeerinfo: address, traffic counters, ping,
 * protocol version/subversion, direction, and ban score. */
struct peerinfo_t {
    std::string addr;
    std::string services;
    int lastsend;
    int lastrecv;
    int bytessent;
    int bytesrecv;
    int conntime;
    double pingtime;
    int version;
    std::string subver;
    bool inbound;
    int startingheight;
    int banscore;
};


/* === Account, address types === */
/* Base record for received-by listings: account label, amount, confirmations. */
struct accountinfo_t {
    std::string account;
    double amount;
    int confirmations;
};

/* accountinfo_t extended with the address and the txids that paid it
 * (listreceivedbyaddress). */
struct addressinfo_t : accountinfo_t {
    std::string address;
    std::vector<std::string> txids;
};

/* Result of getaddressinfo: script/witness flags, ownership flags, and labels
 * for a single address. */
struct getaddressinfo_t {
    std::string address;
    std::string scriptPubKey;
    bool ismine;
    bool iswatchonly;
    bool isscript;
    bool iswitness;
    std::vector<std::string> labels;
};

/* One wallet transaction row (listtransactions / listsinceblock): category,
 * block placement, txid, conflicts, and timestamps. */
struct transactioninfo_t : accountinfo_t {
    std::string address;
    std::string category;
    std::string blockhash;
    int blockindex;
    int blocktime;
    std::string txid;
    std::vector<std::string> walletconflicts;
    int time;
    int timereceived;
};

/* A multisig address and its redeem script (addmultisigaddress/createmultisig). */
struct multisig_t {
    std::string address;
    std::string redeemScript;
};

/* Result of validateaddress: validity and ownership/script flags for an
 * address plus its pubkey. */
struct validateaddress_t {
    bool isvalid;
    std::string address;
    bool ismine;
    bool isscript;
    std::string pubkey;
    bool iscompressed;
    std::string account;
};

/* One address/balance/account row from listaddressgroupings. */
struct addressgrouping_t {
    std::string address;
    double balance;
    std::string account;
};

/* === Transactions === */
/* One detail entry inside a gettransaction result (per-address debit/credit). */
struct transactiondetails_t {
    std::string account;
    std::string address;
    std::string category;
    double amount;
    int vout;
    double fee;
};

/* Result of gettransaction: totals, block placement, timestamps, the per-detail
 * breakdown, and the raw hex. */
struct gettransaction_t {
    double amount;
    double fee;
    int confirmations;
    std::string blockhash;
    int blockindex;
    int blocktime;
    std::string txid;
    std::vector<std::string> walletconflicts;
    int time;
    int timereceived;
    std::vector<transactiondetails_t> details;
    std::string hex;
};

/* Result of decodescript: disassembled script (assm), type, P2SH wrapper
 * address, required signatures, and addresses. (assm holds the "asm" field,
 * renamed to avoid the reserved word.) */
struct decodescript_t {
    std::string assm;
    std::string type;
    std::string p2sh;

    int reqSigs;
    std::vector<std::string> addresses;
};

/* decoderawtransaction return type */
/* An input's unlocking script: disassembled form (assm) and hex. */
struct scriptSig_t {
    std::string assm;
    std::string hex;
};

/* An output's locking script: disassembled form (assm), hex, required sigs,
 * type, and the addresses it pays. */
struct scriptPubKey_t {
    std::string assm;
    std::string hex;
    int reqSigs;
    std::string type;
    std::vector<std::string> addresses;
};

/* A transaction outpoint: the referenced txid and output index n. Base for
 * several richer input/output types. */
struct txout_t {
    std::string txid;
    unsigned int n;
};

/* A decoded transaction input: the outpoint it spends (txid/n), its scriptSig,
 * segwit witness stack, and sequence number. */
struct vin_t : txout_t {
    scriptSig_t scriptSig;
    std::vector<std::string> txinwitness;
    unsigned int sequence;
};

/* A decoded transaction output: value in DGB (value) and in sats (valueS),
 * output index n, and its scriptPubKey. */
struct vout_t {
    double value;
    uint64_t valueS;
    unsigned int n;
    scriptPubKey_t scriptPubKey;
};

/* Result of decoderawtransaction: identifiers, sizes/weight, version/locktime,
 * and the decoded input/output lists. */
struct decoderawtransaction_t {
    std::string txid;
    std::string hash;
    unsigned int size;
    unsigned int vsize;
    unsigned int weight;
    int version;
    int locktime;
    std::vector<vin_t> vin;
    std::vector<vout_t> vout;
};


/* getrawtransaction return type */
/* decoderawtransaction_t plus the raw hex and confirmation/block context added
 * by getrawtransaction in verbose mode. */
struct getrawtransaction_t : decoderawtransaction_t {
    std::string hex;
    std::string blockhash;
    unsigned int confirmations;
    unsigned int time;
    unsigned int blocktime;
};

/* signrawtransaction input hint: an outpoint (txid/n) with the scriptPubKey and
 * optional redeemScript the signer needs. */
struct signrawtxin_t : txout_t {
    std::string scriptPubKey;
    std::string redeemScript;
};

/* signrawtransaction return type */
/* Result of signrawtransaction: the (possibly partially) signed hex and whether
 * signing is complete. */
struct signrawtransaction_t {
    std::string hex;
    bool complete;
};


/* === Other === */
/* Result of gettxout: the UTXO's best-block context, confirmations, value,
 * script, version, and coinbase flag. */
struct utxoinfo_t {
    std::string bestblock;
    int confirmations;
    double value;
    scriptPubKey_t scriptPubKey;
    int version;
    bool coinbase;
};

/* Result of gettxoutsetinfo: aggregate UTXO-set statistics at a given height. */
struct utxosetinfo_t {
    int height;
    std::string bestblock;
    int transactions;
    int txouts;
    int bytes_serialized;
    std::string hash_serialized;
    double total_amount;
};

/* One spendable output from listunspent: outpoint (txid/n) plus its address,
 * account, script, amount, and confirmations. */
struct unspenttxout_t : txout_t {
    std::string address;
    std::string account;
    std::string scriptPubKey;
    double amount;
    int confirmations;
};


/* === Unused yet === */
/* Result of getblock: header fields, DigiByte's multi-algo id (algo), the list
 * of txids, and links to neighbouring blocks. */
struct blockinfo_t {
    std::string hash;
    int confirmations;
    int size;
    int strippedsize;
    int weight;
    int height;
    int version;
    unsigned char algo;
    std::string merkleroot;
    std::vector<std::string> tx;
    unsigned int time;
    unsigned int nonce;
    std::string bits;
    double difficulty;
    std::string chainwork;
    std::string previousblockhash;
    std::string nextblockhash;
};

/* Result of getmininginfo: block/difficulty stats, network hashrate, mempool
 * size, and generation state. */
struct mininginfo_t {
    int blocks;
    int currentblocksize;
    int currentblocktx;
    double difficulty;
    std::string errors;
    int genproclimit;
    double networkhashps;
    int pooledtx;
    bool testnet;
    bool generate;
    int hashespersec;
};

/* Result of getwork: the mining work package (midstate, data, hash1, target). */
struct workdata_t {
    std::string midstate;
    std::string data;
    std::string hash1;
    std::string target;
};

/* Result of listsinceblock: wallet transactions since a block plus the hash of
 * the last block scanned. */
struct txsinceblock_t {
    std::vector<transactioninfo_t> transactions;
    std::string lastblock;
};

#endif //DIGIASSET_CORE_DIGIBYTECORE_TYPES_H
