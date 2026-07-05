//
// Created by mctrivia on 15/06/23.
//
// Implementation of the IPFS controller (see IPFS.h). Contains the background
// worker loop that executes queued download/pin/unpin jobs against the local
// Kubo node's HTTP API, the async/sync request entry points, the pure CID and
// URL helpers, the hard-coded "known lost" CID skip list, and the peer-address
// discovery logic used by getPeerId() to advertise this node to the pool.
//

#include "IPFS.h"
#include "AppMain.h"
#include "Config.h"
#include "CurlHandler.h"
#include "Database.h"
#include <fstream>
#include <iostream>
#include <regex>

using namespace std;
using namespace chrono;


///This is a list of CID we happen to know have been lost to time.  To save time we won't bother looking for these.
///If anyone ever finds any of these please let us know so we can pin and remove from the list.
///This list must be in alphabetical order
const std::vector<std::string> IPFS::_knownLostCID = {
        "bafkreiabavnsbsrrlfgisxcgmd7ontytbyh2ilruux7gjfc2hzi4qa5vxy",
        "bafkreiaefjrafwopa4skec2ks3345qkkrgokut6ojvzmufrxu6voubxel4",
        "bafkreiamu6dvkns57pyy7ahuuirtuivshwjwimn5p35cc5zshkvqbna4hy",
        "bafkreianpe4ru6qo27rvv6nom3hamrfzdfljet5gnp2ui6cvagymlwypuy",
        "bafkreib5elfpb43h57cdply5yxweqktsz4ocmrvsidtxffehgfemgi5hwi",
        "bafkreib74tvcm2sd2th5xxop4cont3fxncg75gnnd4dj4ulypn6w5zlh7q",
        "bafkreibc7epd3jpuzg5i2bkdwotiase6qgomlrby6l2gar5cmmye3jpope",
        "bafkreibodwn2cvf6axsfjoung2wfzcq62nayponfnmaxy7bxvmpx7hqqjq",
        "bafkreibsziw2nmgfin5pgxdjzua6yiq3plyp7kya4haxnnra5kla2mdkyu",
        "bafkreibziyku5zy2pdehywvfiynw5o5whdghowhzbwul7psskx3nfc3w4e",
        "bafkreic42xxy2rhziizosvrsitc6ktbiw6gsbxc355kd3g5x2bzibubgse",
        "bafkreic4etklteiiikqrk4yfpofhv2vj6n3m2nqyucwpp2hbupwm3tioxy",
        "bafkreic6xez3wmv3xfjzlpyi22t2vkrs3kgqmvh2bvpqgxv2q36ulkr7em",
        "bafkreidjxv2jwj4ye7ts7f3o3i2q5uxuoiyczwbofu7l5g6qr74yrd5l5i",
        "bafkreidkis7kzj6ymwl247mxhu7ggi334y6lkw3gtxf7q3pstl2rkbsxxq",
        "bafkreie4zpsvel5kdjvg7nu6b7btnddhabmkwekfr36y57wepkb5xhrfn4",
        "bafkreiemvr6co2j6njnfrbdz5nkdtjxlge5wfszycquuukxsdsl3scduvy",
        "bafkreieq4vf22xsxmkch57e6ki4xp7opznnnu3cskbphxaw65lh7qmlici",
        "bafkreiesdfqhnoxtpphota7f32yhtroqwuxguqfotjmuvto3bv34li2hne",
        "bafkreif4adzecnprye3klsz7yim7zrqhdikdlhrv75uk5z2jnlvwkqe2ui",
        "bafkreifdp4xmaz6ymjakya7lfjhmzwf267z4coijhe362ukd5fdzdq37ya",
        "bafkreificfukeremoic7uenn75njz42i2eorcznzyqp2wfyla4c23fgou4",
        "bafkreig4ebwjbzqwvgdqi6fxyjel4h7ysgrylpglvctfw56r3p2d6abnvq",
        "bafkreigs522vgiqvsr66qclwdlhoiubz3mgtewxxqom7ebfntnc6oary3i",
        "bafkreihedtic7vpwogtetqhuhpc4ujbousv3oaimxcshs5e2iyarvr7btq",
        "bafkreihglxkvgwkgw7qzr2qg5s7ctientywybvntnb64lp4s5jv5hj7srm",
        "bafkreihpexqyn25gicpfn73sggqtmz5mxk6tambf2wbjs2ydqchylzon44",
        "bafkreihvwi6b6xxedihdqolm46i5e5znol6hiiv6h6irlx2wqgealv454a"};

/*
 ██████╗ ██████╗ ███╗   ██╗███████╗████████╗██████╗ ██╗   ██╗ ██████╗████████╗██╗ ██████╗ ███╗   ██╗
██╔════╝██╔═══██╗████╗  ██║██╔════╝╚══██╔══╝██╔══██╗██║   ██║██╔════╝╚══██╔══╝██║██╔═══██╗████╗  ██║
██║     ██║   ██║██╔██╗ ██║███████╗   ██║   ██████╔╝██║   ██║██║        ██║   ██║██║   ██║██╔██╗ ██║
██║     ██║   ██║██║╚██╗██║╚════██║   ██║   ██╔══██╗██║   ██║██║        ██║   ██║██║   ██║██║╚██╗██║
╚██████╗╚██████╔╝██║ ╚████║███████║   ██║   ██║  ██║╚██████╔╝╚██████╗   ██║   ██║╚██████╔╝██║ ╚████║
 ╚═════╝ ╚═════╝ ╚═╝  ╚═══╝╚══════╝   ╚═╝   ╚═╝  ╚═╝ ╚═════╝  ╚═════╝   ╚═╝   ╚═╝ ╚═════╝ ╚═╝  ╚═══╝
*/
/**
 * Constructor. Reads IPFS settings (node URL prefix, pin/download/retry
 * timeouts, parallel-job count) from configFile, applying defaults for anything
 * absent, and starts the worker thread unless runStart is false.
 */
IPFS::IPFS(const string& configFile, bool runStart) {
    Config config(configFile);
    //todo check _nodePrefix works before setting
    _nodePrefix = config.getString("ipfspath", "http://localhost:5001/api/v0/");
    _timeoutPin = config.getInteger("ipfstimeoutpin", 1200);
    _timeoutDownload = config.getInteger("ipfstimeoutdownload", 3600);
    _timeoutRetry = config.getInteger("ipfstimeoutretry", 3600);
    setMaxParallels(config.getInteger("ipfsparallel", 10));
    if (runStart) start();
}

/*
████████╗██╗  ██╗██████╗ ███████╗ █████╗ ██████╗
╚══██╔══╝██║  ██║██╔══██╗██╔════╝██╔══██╗██╔══██╗
   ██║   ███████║██████╔╝█████╗  ███████║██║  ██║
   ██║   ██╔══██║██╔══██╗██╔══╝  ██╔══██║██║  ██║
   ██║   ██║  ██║██║  ██║███████╗██║  ██║██████╔╝
   ╚═╝   ╚═╝  ╚═╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═════╝
*/
/**
 * Worker-thread body, invoked repeatedly by the Threaded base class. Pulls the
 * next queued IPFS job from the Database; if none, sleeps 100ms and returns.
 * Skips (and removes) jobs for invalid or known-lost CIDs. Otherwise it either
 * pins/unpins the CID (honoring the optional max-size limit in extra) or
 * downloads it (cat) subject to the download timeout. On a non-final download
 * timeout it re-queues the job for a later retry (leaving the callback
 * unfired); otherwise it removes the job and invokes the job's callback with
 * the content and a failed flag.
 */
void IPFS::mainFunction() {
    //get next job if there is one
    Database* db = AppMain::GetInstance()->getDatabase();
    unsigned int jobIndex;
    string cid;
    string sync;
    string extra;
    unsigned int maxSleep; //ms
    IPFSCallbackFunction callback;
    db->getNextIPFSJob(jobIndex, cid, sync, extra, maxSleep, callback);
    if (jobIndex == 0) {
        //no waiting jobs so pause briefly before checking again
        chrono::milliseconds dura(100);
        this_thread::sleep_for(dura);
        return;
    }

    //check if it is a known bad cid
    if (!isValidCID(cid) || isLostCID(cid)) {
        //since this can only happen do to the repin sql statement pinning stuff that should not have been pinned just remove the job
        db->removeIPFSJob(jobIndex, sync);
        return;
    }

    //download the cid
    string content;
    bool failed = false;
    if (sync == "pin") {
        try {
            //check if max size exceeded
            if (
                    extra.empty() ||              //no restrictions
                    (getSize(cid) < stoul(extra)) //within restrictions
            ) {
                _command("pin/add/" + cid, {}, _timeoutPin * 1000);
            }
        } catch (...) {
            //don't worry about failed pin (timeout, size check failure, etc.)
        }
    } else if (sync == "unpin") {
        try {
            //check if max size exceeded
            if (
                    extra.empty() ||              //no restrictions
                    (getSize(cid) < stoul(extra)) //within restrictions
            ) {
                _command("pin/rm/" + cid, {}, _timeoutPin * 1000);
            }
        } catch (...) {
            //don't worry about failed unpin
        }
    } else {
        //figure out what the max time we should try to download the file for is
        unsigned int timeout = _timeoutDownload * 1000;
        bool lastTry = false;
        if (maxSleep < timeout) {
            timeout = maxSleep;
            lastTry = true;
        }

        //download the file
        try {
            content = _command("cat/" + cid, {}, timeout);
        } catch (const exceptionTimeout& e) {
            if (!lastTry) {
                db->pauseIPFSSync(jobIndex, sync, _timeoutRetry * 1000);
                return; //don't remove job or make callback
            }
            failed = true;
        }
    }

    //remove job
    db->removeIPFSJob(jobIndex, sync);

    //execute that functions call back
    callback(cid, extra, content, failed);
}


/*
██╗  ██╗███████╗██╗     ██████╗ ███████╗██████╗ ███████╗
██║  ██║██╔════╝██║     ██╔══██╗██╔════╝██╔══██╗██╔════╝
███████║█████╗  ██║     ██████╔╝█████╗  ██████╔╝███████╗
██╔══██║██╔══╝  ██║     ██╔═══╝ ██╔══╝  ██╔══██╗╚════██║
██║  ██║███████╗███████╗██║     ███████╗██║  ██║███████║
╚═╝  ╚═╝╚══════╝╚══════╝╚═╝     ╚══════╝╚═╝  ╚═╝╚══════╝
 */


/**
 * Converts a SHA256 hash to IPFS cid
 * This works for data that was encoded in raw mode only which has a file size limit of 2MB
 * @param hash
 * @return
 */
string IPFS::sha256ToCID(BitIO& hash) {
    if (hash.getLength() != 256) throw out_of_range("Invalid Hash Size");
    const char chars[33] = "abcdefghijklmnopqrstuvwxyz234567";

    //encode binary data
    BitIO data;
    data.appendBits(0x01551220, 32); //header
    data.appendBits(hash);
    data.appendZeros(2);
    data.movePositionToBeginning();

    //encode in base 32
    string output = "b"; //b means base 32
    for (size_t i = 0; i < 58; i++) output.push_back(chars[data.getBits(5)]);
    return output;
}

/**
 * Converts a SHA256 hash to IPFS cid
 * This works for data that was encoded in raw mode only which has a file size limit of 2MB
 * @param hash
 * @return
 */
string IPFS::sha256ToCID(const string& hash) {
    BitIO data = BitIO::makeHexString(hash);
    return sha256ToCID(data);
}


/**
 * Sends a command to the IPFS node and return result
 * @param command - should be in the format cat/cid  should not have a / at beginning
 * @param data
 * @param timeout - max number of mc to allow timeout
 * @param outputPath - if present will treat result as binary data and save it to a file
 * @return
 */
string IPFS::_command(const string& command, const map<string, string>& data, unsigned int timeout, const string& outputPath) const {
    string url = _nodePrefix + command;
    try {
        if (outputPath.empty()) return CurlHandler::post(url, data, timeout);
        CurlHandler::postDownload(url, outputPath, data, timeout);
    } catch (const CurlHandler::exceptionTimeout& e) {
        //replace CurlHandler error with IPFS error
        throw exceptionTimeout();
    } catch (const std::exception& e) {
        if (string(e.what()) == "Couldn't connect to server" ||
            string(e.what()) == "Could not connect to server")
                throw exceptionNoConnection();
        throw;
    }
    return "";
}


/**
 * Gets the users current IP address
 */
string IPFS::getIP() {
    vector<string> ipSources = {
            "https://api.ipify.org/",
            "https://icanhazip.com/",
            "https://api6.ipify.org/"};

    for (const auto& url: ipSources) {
        try {
            string ip = CurlHandler::get(url);
            if (ip.empty()) continue;
            return ip;
        } catch (const runtime_error& e) {
        }
    }
    throw exception();
}

/**
 * From the node's advertised multiaddrs, pick the single best one a remote peer
 * can use to reach this node, given our detected WAN ip. Tries, in order:
 *   Priority 1 - a direct TCP address that already contains our WAN IP
 *                (lowest port if several).
 *   Priority 2 - a routable p2p-circuit relay address, preferring plain-TCP
 *                IPv4, then plain-TCP IPv6, then DNS, then anything else;
 *                loopback/link-local/RFC1918 relays are skipped.
 *   Priority 3 - last resort: substitute our WAN IP into a local ip4/ip6 TCP
 *                address (only works if the port is actually forwarded).
 * Returns the chosen multiaddr, or "" if nothing qualifies.
 */
string IPFS::findPublicAddress(const vector<string>& addresses, const string& ip) {
    // Priority 1: a real TCP address that already contains our WAN IP.
    // This is the happy path: the node is directly reachable at ip:port.
    {
        vector<string> direct;
        regex tcpRegex(".*tcp.*");
        regex ipRegex(".*" + ip + ".*");
        for (const auto& addr: addresses) {
            if (regex_match(addr, tcpRegex) && regex_match(addr, ipRegex)) {
                direct.push_back(addr);
            }
        }
        if (!direct.empty()) {
            if (direct.size() == 1) return direct[0];
            //multiple — pick lowest port
            string best;
            int lowest = 65536;
            regex portRegex("tcp/([0-9]*)/");
            for (const auto& addr: direct) {
                smatch match;
                if (regex_search(addr, match, portRegex)) {
                    int port = stoi(match[1].str());
                    if (port < lowest) {
                        best = addr;
                        lowest = port;
                    }
                }
            }
            return best;
        }
    }

    // Priority 2: a real p2p-circuit relay address. The node is behind NAT
    // but IPFS has negotiated a relay — libp2p peers (including mctrivia's
    // server) can dial the node THROUGH the relay using this exact multiaddr.
    // We never fabricate these; we use them verbatim from the /id response.
    //
    // Preference order within relay addresses:
    //   1. /ip4/PUBLIC/tcp/PORT/p2p/RELAY/p2p-circuit/p2p/SELF  — plain TCP, most universal
    //   2. /ip6/PUBLIC/tcp/PORT/...                              — IPv6 plain TCP
    //   3. /dns4/.../tcp/PORT/...                                — DNS-based (may use TLS/WSS)
    //   4. anything else with /p2p-circuit/ and /tcp/
    // Skip loopback, link-local, and RFC1918 relays (not routable from the internet).
    auto isUnroutableRelay = [](const string& addr) -> bool {
        if (addr.find("/127.0.0.1/") != string::npos) return true;
        if (addr.find("/::1/") != string::npos) return true;
        if (addr.find("/ip4/10.") != string::npos) return true;
        if (addr.find("/ip4/192.168.") != string::npos) return true;
        if (addr.find("/ip4/172.16.") != string::npos) return true;
        if (addr.find("/ip4/172.17.") != string::npos) return true;
        if (addr.find("/ip4/172.18.") != string::npos) return true;
        if (addr.find("/ip4/172.19.") != string::npos) return true;
        if (addr.find("/ip4/172.2") != string::npos) return true;   // 172.20–172.29
        if (addr.find("/ip4/172.30.") != string::npos) return true;
        if (addr.find("/ip4/172.31.") != string::npos) return true;
        if (addr.find("/ip4/169.254.") != string::npos) return true; // link-local
        return false;
    };
    auto isPlainTcpRelay = [](const string& addr) -> bool {
        // Plain TCP = no /tls/, /ws/, /wss/, /quic/, /webtransport/, /webrtc/ segments before /p2p-circuit/
        size_t circuit = addr.find("/p2p-circuit/");
        if (circuit == string::npos) return false;
        string pre = addr.substr(0, circuit);
        if (pre.find("/tls/") != string::npos) return false;
        if (pre.find("/ws/") != string::npos) return false;
        if (pre.find("/wss/") != string::npos) return false;
        if (pre.find("/quic") != string::npos) return false;
        if (pre.find("/webtransport") != string::npos) return false;
        if (pre.find("/webrtc") != string::npos) return false;
        return true;
    };

    vector<string> relayIp4Plain, relayIp6Plain, relayDns, relayOther;
    for (const auto& addr: addresses) {
        if (addr.find("/p2p-circuit/") == string::npos) continue;
        if (addr.find("/tcp/") == string::npos) continue;
        if (isUnroutableRelay(addr)) continue;

        bool plain = isPlainTcpRelay(addr);
        if (addr.rfind("/ip4/", 0) == 0 && plain)       relayIp4Plain.push_back(addr);
        else if (addr.rfind("/ip6/", 0) == 0 && plain)  relayIp6Plain.push_back(addr);
        else if (addr.rfind("/dns", 0) == 0)            relayDns.push_back(addr);
        else                                             relayOther.push_back(addr);
    }
    if (!relayIp4Plain.empty()) return relayIp4Plain.front();
    if (!relayIp6Plain.empty()) return relayIp6Plain.front();
    if (!relayDns.empty())       return relayDns.front();
    if (!relayOther.empty())     return relayOther.front();

    // Priority 3: last-resort fabrication (original behavior). Substitute our
    // WAN IP into a local address. This only works if port 4001 is actually
    // forwarded on the user's router; otherwise the server can't dial back.
    vector<string> possible;
    regex constructRegex("^(.*ip[46]/)([^/]*)/(tcp.*$)");
    smatch match;
    for (const auto& addr: addresses) {
        if (regex_match(addr, match, constructRegex)) {
            possible.push_back(match[1].str() + ip + "/" + match[3].str());
        }
    }
    if (possible.size() == 1) return possible[0];
    string peerId;
    int lowest = 65536;
    regex portRegex("tcp/([0-9]*)/");
    for (const auto& addr: possible) {
        smatch m;
        if (regex_search(addr, m, portRegex)) {
            int port = stoi(m[1].str());
            if (port < lowest) {
                peerId = addr;
                lowest = port;
            }
        }
    }
    return peerId;
}

/**
 * Parses the JSON body of an /id response and returns its list of multiaddrs
 * from the "Addresses" field (accepting lowercase "addresses" as a fallback).
 * Returns an empty vector if the JSON is malformed or the field is absent.
 */
vector<string> IPFS::extractAddresses(const string& idString) {
    Json::Value root;
    Json::Reader reader;
    vector<string> addresses;

    if (reader.parse(idString, root)) {
        const Json::Value& addrs = root.isMember("Addresses") ? root["Addresses"] : root["addresses"];
        for (const auto& addr: addrs) {
            addresses.push_back(addr.asString());
        }
    }

    return addresses;
}

/**
 * Parses the /id response and returns the bare "ID" field (this node's own
 * peerId, e.g. "12D3KooW..." or "Qm..."). Accepts lowercase "id" as a
 * fallback. Returns "" if the field is missing or the JSON is malformed.
 */
string IPFS::extractIdField(const string& idString) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(idString, root)) return "";
    if (root.isMember("ID") && root["ID"].isString()) return root["ID"].asString();
    if (root.isMember("id") && root["id"].isString()) return root["id"].asString();
    return "";
}


/*
 ██████╗ █████╗ ██╗     ██╗     ██████╗  █████╗  ██████╗██╗  ██╗███████╗
██╔════╝██╔══██╗██║     ██║     ██╔══██╗██╔══██╗██╔════╝██║ ██╔╝██╔════╝
██║     ███████║██║     ██║     ██████╔╝███████║██║     █████╔╝ ███████╗
██║     ██╔══██║██║     ██║     ██╔══██╗██╔══██║██║     ██╔═██╗ ╚════██║
╚██████╗██║  ██║███████╗███████╗██████╔╝██║  ██║╚██████╗██║  ██╗███████║
 ╚═════╝╚═╝  ╚═╝╚══════╝╚══════╝╚═════╝ ╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝╚══════╝
 */

void IPFS::registerCallback(const string& callbackSymbol, const IPFSCallbackFunction& callback) {
    Database::registerIPFSCallback(callbackSymbol, callback);
}


/*
██╗███╗   ██╗████████╗███████╗██████╗ ███████╗ █████╗  ██████╗███████╗
██║████╗  ██║╚══██╔══╝██╔════╝██╔══██╗██╔════╝██╔══██╗██╔════╝██╔════╝
██║██╔██╗ ██║   ██║   █████╗  ██████╔╝█████╗  ███████║██║     █████╗
██║██║╚██╗██║   ██║   ██╔══╝  ██╔══██╗██╔══╝  ██╔══██║██║     ██╔══╝
██║██║ ╚████║   ██║   ███████╗██║  ██║██║     ██║  ██║╚██████╗███████╗
╚═╝╚═╝  ╚═══╝   ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝  ╚═╝ ╚═════╝╚══════╝
*/
/**
 * Function to download data from IPFS and run a pre registered callback when done.
 * If sync is "" call back may be executed immediately if data already downloaded.
 * Is sync provided will always execute all values with the same sync value in order.
 * @param cid - cid of file you want downloaded
 * @param sync - "" to specify order execution does not matter.  all values of same sync value otherwise executed in order added
 * @param extra - any value you want passed to callback
 * @param callbackRegistry - preregistered value with IPFS::registerCallback function
 * @param maxTime - max time in ms to wait for IPFS data before throwing an exception
 */
void IPFS::callOnDownload(const string& cid, const string& sync, const string& extra,
                          const string& callbackRegistry, unsigned int maxTime) {
    //check if no cid
    if (!isValidCID(cid)) return;
    if (isLostCID(cid)) return; //this function never throws errors

    Database* db = AppMain::GetInstance()->getDatabase();

    //check if we can do synchronously quickly
    if (sync.empty() && isPinned(cid)) {
        try {
            string content = _command("cat/" + cid);
            db->getIPFSCallback(callbackRegistry)(cid, extra, content, false);
        } catch (...) {
            //this function makes the request and does not wait for a response.
            //If asynchronous exceptions can't be handled, so we will ignore if synchronous, so it responds the same both ways
        }
        return;
    }

    //add type download to database
    db->addIPFSJob(cid, sync, extra, maxTime, callbackRegistry);
}


/**
 * Downloads a value from IPFS and returns a promise
 * @param cid - cid of file you want downloaded
 * @param sync - "" to specify order execution does not matter.  all values of same sync value otherwise executed in order added
 * @param maxTime - max time in ms to wait for IPFS data before throwing an exception
 * @return
 */
promise<string> IPFS::callOnDownloadPromise(const string& cid, const string& sync, unsigned int maxTime) {
    //check if a known lost CID
    if (!isValidCID(cid)) {
        promise<string> result;
        result.set_exception(std::make_exception_ptr(exceptionInvalidCID(cid)));
        return result;
    }
    if (isLostCID(cid)) {
        promise<string> result;
        result.set_exception(std::make_exception_ptr(exceptionTimeout()));
        return result;
    } //well it would have timed out if we had let it

    //check if we can do synchronously quickly
    if (sync.empty() && isPinned(cid)) {
        promise<string> result;
        try {
            string content = _command("cat/" + cid);
            result.set_value(content);
        } catch (const std::exception& e) {
            // If an error occurs, set the exception
            result.set_exception(std::make_exception_ptr(e));
        }
        return result;
    }

    //run asynchronously
    Database* db = AppMain::GetInstance()->getDatabase();
    return db->addIPFSJobPromise(cid, sync, maxTime);
}

/**
 * Downloads a value from IPFS and returns it.  Code execution will stop until download is complete
 * @param cid - cid of file you want downloaded
 * @param sync - "" to specify order execution does not matter.  all values of same sync value otherwise executed in order added
 * @param maxTime - max time in ms to wait for IPFS data before throwing an exception
 * @return
 */
std::string IPFS::callOnDownloadSync(const string& cid, const string& sync, unsigned int maxTime) {
    return callOnDownloadPromise(cid, sync, maxTime).get_future().get();
}

/**
 * Pins a file
 * @param cid
 * @param maxSize - optional 0=don't pin, 1=pin no matter size, >1 pin only if up to that size(0 an option to allow easy config disabling of download)
 */
void IPFS::pin(const string& cid, unsigned int maxSize) {
    //check if no cid
    if (!isValidCID(cid)) return;                 //just ignore bad cids for pin requests
    if (isLostCID(cid)) throw exceptionTimeout(); //well it would have timed out if we had let it
    if (maxSize == 0) return;                     //skip because set to not pin

    //compute extra
    string extra = (maxSize == 1) ? "" : to_string(maxSize); //if max size is 1 than we don't process the size

    //add type download to database
    Database* db = AppMain::GetInstance()->getDatabase();
    db->addIPFSJob(cid, "pin", extra);
}

void IPFS::unpin(const string& cid) {
    //check if no cid
    if (!isValidCID(cid)) return; //just ignore bad cids for pin requests

    //add type download to database
    Database* db = AppMain::GetInstance()->getDatabase();
    db->addIPFSJob(cid, "unpin");
}



bool IPFS::isPinned(const string& cid) const {
    string results = _command("pin/ls/" + cid);
    return (results.find("is not pinned") == string::npos);
}

unsigned int IPFS::getSize(const string& cid) const {
    if (!isValidCID(cid)) throw exceptionInvalidCID(cid);
    if (isLostCID(cid)) throw exceptionTimeout(); //well it would have timed out if we had let it
    string stats = _command("object/stat?arg=" + cid);
    Json::Value json;
    Json::CharReaderBuilder rbuilder;
    istringstream s(stats);
    string errs;
    if (!Json::parseFromStream(rbuilder, s, &json, &errs)) throw out_of_range("No size data found");

    if (json.isMember("CumulativeSize") && json["CumulativeSize"].isInt()) {
        return json["CumulativeSize"].asUInt();
    }
    // Handle error case
    throw out_of_range("No size data found");
}

/**
 * Synchronously downloads a file.
 * Strongly recommend you pin the file and check it is pinned before using this function since it will cause thread to hang for a long time if not pinned
 * @param cid - cid of file to download
 * @param filePath - location file will be saved
 * @param pinAlso - defaults false.  Set to true for downloading startup files that must be present for program to run
 */
void IPFS::downloadFile(const string& cid, const string& filePath, bool pinAlso) {
    if (!isValidCID(cid)) throw exceptionInvalidCID(cid);
    if (isLostCID(cid)) throw exceptionTimeout(); //well it would have timed out if we had let it
    if (pinAlso) _command("pin/add/" + cid);
    _command("cat?arg=" + cid, {}, 0, filePath);
}

/**
 * Function that determines if a url is pointing to a file on IPFS
 * @param url
 * @return
 */
bool IPFS::isIPFSurl(const string& url) {
    const char* prefix = "ipfs://";
    const size_t prefixLength = 7; // Length of "ipfs://"

    // Check if the input string is at least as long as the prefix
    if (url.length() < prefixLength) {
        return false;
    }

    // Compare the characters case-insensitively
    for (size_t i = 0; i < prefixLength; ++i) {
        if (tolower(url[i]) != prefix[i]) {
            return false;
        }
    }

    //check if remainder is a valid cid
    return isValidCID(url.substr(prefixLength));
}
/**
 * Determines the multiaddr the pool server should use to reach this node.
 * Detects our WAN IP (getIP), queries the local node's /id for its advertised
 * addresses, and filters those to only ones ending in "/p2p/<ourId>" (direct or
 * via a /p2p-circuit relay) so a bootstrap peer's address is never mistaken for
 * ours. Runs the surviving addresses through findPublicAddress. Falls back to
 * the bare peerId if nothing in /id clearly identifies us, or to the unfiltered
 * findPublicAddress result if the local ID could not be parsed.
 */
string IPFS::getPeerId() const {
    //get this machines ip address
    std::string ip = getIP();

    //get list of addresses for the ipfs node
    std::string idString = _command("id");
    auto addresses = extractAddresses(idString);

    // Filter out any address whose trailing "/p2p/<id>" segment does NOT
    // match the local node's own ID. Without this filter, the upstream
    // /id response can contain addresses of peers the local Kubo is
    // connected to (especially on older Kubo versions or when Relay is
    // enabled) and findPublicAddress's priority-3 fabrication path can
    // accidentally return a bootstrap node's multiaddr as "ours".
    // See memory/project_peerid_bootstrap_bug.md for the history.
    std::string localId = extractIdField(idString);
    if (!localId.empty()) {
        const std::string marker = "/p2p/" + localId;
        std::vector<std::string> filtered;
        for (const auto& addr: addresses) {
            // An address is "ours" if it contains /p2p/<localId>, either at
            // the end (direct) or in the middle with a /p2p-circuit/ relay
            // after it (e.g. /ip4/X/tcp/Y/p2p/RELAY/p2p-circuit/p2p/US).
            // Accept both cases — the final /p2p/<localId> must be present.
            // Simplest check: address ENDS in marker, OR contains marker
            // preceded by /p2p-circuit.
            if (addr.size() >= marker.size() &&
                addr.compare(addr.size() - marker.size(), marker.size(), marker) == 0) {
                filtered.push_back(addr);
            }
        }
        if (!filtered.empty()) {
            return findPublicAddress(filtered, ip);
        }
        // Nothing in the /id response clearly identifies us. Fall back to
        // returning just the bare peerId so at least the pool server has
        // a stable identifier for this node.
        return localId;
    }

    //find the one that best represents the public ip address
    return findPublicAddress(addresses, ip);
}

/**
 * Returns the CID from an IPFS url.  Will throw an error if not an IPFS url
 * @param url
 * @return
 */
std::string IPFS::getCID(const string& url) {
    if (!isIPFSurl(url)) throw out_of_range("Not an IPFS url");
    return url.substr(7);
}
bool IPFS::isLostCID(const std::string& cid) {
    return std::binary_search(_knownLostCID.begin(), _knownLostCID.end(), cid);
}

/**
 * A bit of a hacky method of removing known invalid CIDs.
 * Would be nice to have this actually validate cid format but this works for all bad cids i have seen so far
 * @param cid
 * @return
 */
bool IPFS::isValidCID(const string& cid) {
    //check if no cid provided
    if (cid.empty()) return false;

    //check only alphanumeric characters
    return std::all_of(cid.begin(), cid.end(), [](unsigned char c) {
        return std::isalnum(c);
    });
}
