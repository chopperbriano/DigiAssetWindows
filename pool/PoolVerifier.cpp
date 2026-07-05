//
// PoolVerifier.cpp - implementation of the pool's background dial-back
// checker. Owns a worker thread (loop()) that periodically pulls the
// least-recently-verified registered peers from PoolDatabase and confirms
// each is still serving content over IPFS, recording success/failure back
// to the database so Phase 3 payouts can be gated on real reachability.
// Talks to the local Kubo node exclusively over its HTTP API via
// CurlHandler. See pool/PoolVerifier.h for the hybrid-verification rationale.
//
#include "PoolVerifier.h"
#include "PoolDatabase.h"
#include "CurlHandler.h"
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
    // URL-encode a peerId or multiaddr so it can be passed as a query arg
    // to the IPFS HTTP API. Only reserved characters need encoding; a bare
    // /p2p/<id> contains '/' which must become %2F.
    std::string urlEscape(const std::string& s) {
        std::ostringstream out;
        out << std::hex << std::uppercase;
        for (unsigned char c: s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                out << c;
            } else {
                out << '%';
                if ((int) c < 16) out << '0';
                out << (int) c;
            }
        }
        return out.str();
    }
}

// Hold a reference to the shared pool database and remember the base URL of
// the local IPFS HTTP API (e.g. http://localhost:5001/api/v0/). Does not
// start the worker thread — call start().
PoolVerifier::PoolVerifier(PoolDatabase& db, const std::string& ipfsApiBase)
    : _db(db), _ipfsApiBase(ipfsApiBase) {
    // Ensure trailing slash so `_ipfsApiBase + "swarm/connect"` is well-formed.
    if (!_ipfsApiBase.empty() && _ipfsApiBase.back() != '/') _ipfsApiBase += '/';
}

PoolVerifier::~PoolVerifier() {
    stop();
}

// Launch the verify loop on a background thread. Idempotent: a second call
// while already running is a no-op.
void PoolVerifier::start() {
    if (_running.exchange(true)) return;
    _thread = std::thread([this]() { this->loop(); });
}

// Signal the loop to exit and join the worker thread. Idempotent and safe to
// call from the destructor.
void PoolVerifier::stop() {
    if (!_running.exchange(false)) return;
    if (_thread.joinable()) _thread.join();
}

// Worker-thread body. Runs until stop(): each ~60s iteration pulls a small
// batch of peers to verify, detects the local node's own peerId (to
// auto-pass same-machine clients that can't be dialed due to NAT
// hairpinning), builds the NAT-tolerant provider fallback set from a few
// sample CIDs (gated on at least one being fetchable), then verifies each
// peer and records the outcome. Sleeps in 1s steps so shutdown is prompt.
void PoolVerifier::loop() {
    // One-time small delay so we don't fire probes before the pool server
    // has even logged "accepting connections". Feels nicer for the log.
    std::this_thread::sleep_for(std::chrono::seconds(5));

    while (_running.load()) {
        // Pull a small batch of peers to verify. Limit 10 means each
        // iteration touches at most 10 peers; with a 60s sleep that's
        // 600 probes/hour against kubo, well within its comfort zone.
        auto peers = _db.getPeerIdsForVerification(10);

        // Detect the local IPFS node's own peerId so we can auto-verify
        // same-machine clients. When the pool server and client run on the
        // same box, swarm/connect to our own WAN IP fails due to NAT
        // hairpinning — but the node is definitionally serving content.
        std::string localPeerId;
        try {
            std::string idResp = CurlHandler::post(_ipfsApiBase + "id", {}, 5000);
            size_t idPos = idResp.find("\"ID\"");
            if (idPos == std::string::npos) idPos = idResp.find("\"id\"");
            if (idPos != std::string::npos) {
                size_t q1 = idResp.find('"', idResp.find(':', idPos) + 1);
                size_t q2 = (q1 != std::string::npos) ? idResp.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos)
                    localPeerId = idResp.substr(q1 + 1, q2 - q1 - 1);
            }
        } catch (...) {}

        // Build the NAT-tolerant fallback state once per iteration: sample a
        // few permanent-list CIDs, collect who the DHT says provides them, and
        // confirm at least one is actually fetchable. If nothing is fetchable
        // we leave `providers` empty so the fallback is disabled and we rely on
        // direct dial only — this stops stale provider records from passing a
        // node when the content (or our own IPFS node) is effectively dead.
        // Also keep the PER-CID provider sets (not just the union) so we can
        // score each node's COVERAGE: of the sampled CIDs, how many does it
        // actually provide? Sample a few more (6) than the old fallback needed
        // so coverage is less noisy; the EMA in updateNodeScores smooths it.
        std::vector<std::set<std::string>> sampleProviderSets;
        std::set<std::string> providers;
        bool contentLive = false;
        try {
            for (const auto& cid: _db.getSampleCids(6)) {
                if (!_running.load()) break;
                if (!contentLive && fetchable(cid)) contentLive = true;
                auto provs = findProviders(cid);
                sampleProviderSets.push_back(provs);
                providers.insert(provs.begin(), provs.end());
            }
        } catch (...) {}
        if (!contentLive) { providers.clear(); sampleProviderSets.clear(); }
        size_t sampleN = sampleProviderSets.size();

        for (const auto& peer: peers) {
            if (!_running.load()) break;
            _probesAttempted.fetch_add(1);

            // Auto-verify if the peer's multiaddr contains the local node's
            // own peerId. Same-machine = serving by definition.
            bool isSelf = false;
            if (!localPeerId.empty() && peer.find(localPeerId) != std::string::npos) {
                isSelf = true;
            }

            bool ok = isSelf ? true : verifyPeer(peer, providers);
            if (ok) {
                _probesSucceeded.fetch_add(1);
                _db.recordVerifySuccess(peer);
            } else {
                _db.recordVerifyFailure(peer);
            }

            // Coverage this round = fraction of sampled CIDs whose provider set
            // includes this peer. Same-machine node serves everything (1.0).
            double coverage = 0.0;
            if (isSelf) {
                coverage = 1.0;
            } else if (sampleN > 0) {
                size_t hits = 0;
                for (const auto& provs: sampleProviderSets) {
                    for (const auto& id: provs) {
                        if (peer.find(id) != std::string::npos || id.find(peer) != std::string::npos) { hits++; break; }
                    }
                }
                coverage = (double) hits / (double) sampleN;
            }
            _db.updateNodeScores(peer, coverage, (isSelf || sampleN > 0), ok);
        }

        // Sleep ~60s between iterations, checking the stop flag every second
        // so shutdown is responsive.
        for (int i = 0; i < 60 && _running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

// Hybrid reachability check for one peer. Returns true if the peer can be
// dialed directly (strongest proof) OR, failing that, appears in the DHT
// provider records (`providers`) for content it should be pinning. An empty
// `providers` set fails closed. Matching is by substring so a bare peerId
// and a /p2p/<id> multiaddr both resolve.
bool PoolVerifier::verifyPeer(const std::string& peerIdOrMultiaddr,
                             const std::set<std::string>& providers) {
    // 1) Strongest proof: can we dial the peer right now?
    if (dialPeer(peerIdOrMultiaddr)) return true;

    // 2) NAT-tolerant fallback: is this peer in the DHT provider records for
    //    content it should be pinning? `providers` is already gated on the
    //    content being fetchable, so an empty set means "don't trust the DHT
    //    this round" and we fail closed. Match by substring so a bare peerId
    //    and a /p2p/<id> multiaddr form both resolve.
    for (const auto& prov: providers) {
        if (!prov.empty() && peerIdOrMultiaddr.find(prov) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Ask the local IPFS node (routing/findprovs) who advertises `cid` in the
// DHT and return the set of provider peerIds. Parses the streamed
// newline-delimited JSON by hand, collecting the non-empty "ID" fields.
// Returns an empty set on any error or empty CID (best-effort, never throws).
std::set<std::string> PoolVerifier::findProviders(const std::string& cid) {
    std::set<std::string> out;
    if (cid.empty()) return out;

    // routing/findprovs streams newline-delimited JSON progress objects; the
    // provider peerIds live in the non-empty "ID" fields of the Responses
    // arrays (the top-level per-object "ID" is empty). 20s cap: findprovs can
    // run long, and we'd rather move on than block the verify loop.
    const std::string url = _ipfsApiBase + "routing/findprovs?arg=" +
                            urlEscape(cid) + "&num-providers=20";
    std::string body;
    try {
        body = CurlHandler::post(url, {}, 20000);
    } catch (...) {
        return out;
    }

    size_t pos = 0;
    const std::string key = "\"ID\":\"";
    while ((pos = body.find(key, pos)) != std::string::npos) {
        pos += key.size();
        size_t end = body.find('"', pos);
        if (end == std::string::npos) break;
        std::string id = body.substr(pos, end - pos);
        if (!id.empty()) out.insert(id);
        pos = end + 1;
    }
    return out;
}

// Return true if `cid` actually resolves over IPFS right now, by catting
// only its first 256 bytes (proves bitswap can retrieve it without
// downloading the whole file). A kubo JSON error envelope or empty body
// means not fetchable. Best-effort; never throws.
bool PoolVerifier::fetchable(const std::string& cid) {
    if (cid.empty()) return false;
    // Pull only the first chunk — we just need to prove the content resolves
    // via bitswap, not download the whole file. On failure kubo returns a JSON
    // error envelope with "Type":"error"; a real file's first bytes won't.
    const std::string url = _ipfsApiBase + "cat?arg=" + urlEscape(cid) + "&length=256";
    std::string body;
    try {
        body = CurlHandler::post(url, {}, 20000);
    } catch (...) {
        return false;
    }
    if (body.empty()) return false;
    if (body.find("\"Type\":\"error\"") != std::string::npos) return false;
    return true;
}

// Direct-dial reachability check: ask the local IPFS node to swarm/connect
// to the peer (normalizing a bare peerId to /p2p/<id> so kubo does a DHT
// lookup). Returns true only on an explicit "success" result; a kubo error
// envelope, empty body, timeout, or ambiguous response counts as failure so
// we never mark a dead node reachable. Best-effort; never throws.
bool PoolVerifier::dialPeer(const std::string& peerIdOrMultiaddr) {
    // Normalize to a multiaddr. If the input already contains '/', assume
    // it's a multiaddr like /ip4/1.2.3.4/tcp/4001/p2p/<id>. If it's a bare
    // peerId, wrap it in /p2p/<id> so kubo's swarm/connect does a DHT lookup.
    std::string target = peerIdOrMultiaddr;
    if (target.find('/') == std::string::npos) {
        target = "/p2p/" + target;
    }

    const std::string url = _ipfsApiBase + "swarm/connect?arg=" + urlEscape(target);

    // swarm/connect has to do a DHT lookup for bare-peerId inputs, which
    // can be slow on cold-cached nodes. 30s is generous; anything longer
    // and we'd rather mark the node as unreachable.
    std::string body;
    try {
        body = CurlHandler::post(url, {}, 30000);
    } catch (...) {
        return false;
    }

    // Kubo returns JSON with a "Strings" array of human-readable results.
    // Success looks like:
    //   {"Strings":["connect 12D3Koo... success"]}
    // Failure looks like:
    //   {"Message":"failed to dial ...","Code":0,"Type":"error"}
    if (body.empty()) return false;
    if (body.find("\"Type\":\"error\"") != std::string::npos) return false;
    if (body.find("success") != std::string::npos) return true;

    // Default: assume failure. We'd rather have a false negative (node gets
    // retried next iteration) than a false positive (marking a dead node as
    // reachable).
    return false;
}
