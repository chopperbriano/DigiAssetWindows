//
// Created by mctrivia on 05/04/24.
//
// RPC::Cache - in-memory response cache for the node's JSON-RPC layer.
// Stores previously computed RPC Responses keyed by a hash of (method name +
// params) so repeat requests can be served without re-querying the chain
// analyzer / database. Entries are invalidated either by chain events
// (new block, new asset issuance, address activity) via the Response's own
// validity rules, or evicted oldest-first when the cache exceeds its size cap.
// Thread-safe: all public operations are guarded by an internal mutex.
//

#ifndef DIGIASSET_CORE_CACHE_H
#define DIGIASSET_CORE_CACHE_H


#include "Response.h"
#include <array>
#include <chrono>
#include <jsoncpp/json/value.h>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <cstring>

namespace RPC {

    // Hash functor for a 32-byte key (SHA256 digest) used by the cache map.
    // Because the key is already a cryptographic hash, its first sizeof(size_t)
    // bytes are copied directly into a size_t and used as the bucket hash.
    struct DirectSizeTHash {
        std::size_t operator()(const std::array<uint8_t, 32>& arr) const {
            static_assert(sizeof(std::size_t) <= 32, "size_t is too small");
            std::size_t hash = 0;
            // Copy the first sizeof(std::size_t) bytes from the array into hash
            memcpy(&hash, arr.data(), sizeof(std::size_t));
            return hash;
        }
    };

    // Equality function for std::array<uint8_t, 32>
    struct ArrayEqual {
        bool operator()(const std::array<uint8_t, 32>& a, const std::array<uint8_t, 32>& b) const {
            return a == b;
        }
    };

    // A single cached RPC result plus the time it was inserted/refreshed.
    // The timestamp drives oldest-first eviction when the cache is over size.
    struct CacheEntry {
        Response response;
        std::chrono::steady_clock::time_point timestamp; // Timestamp to track entry age

        CacheEntry(const Response& response)
            : response(response), timestamp(std::chrono::steady_clock::now()) {}
    };

    // Thread-safe cache mapping SHA256(method+params) -> cached Response.
    // Enforces a soft byte-size budget and exposes invalidation hooks that the
    // node calls when chain state changes.
    class Cache {
    private:
        std::unordered_map<std::array<uint8_t, 32>, CacheEntry, DirectSizeTHash, ArrayEqual> _cacheMap;
        size_t _currentCacheSize = 0;
        const size_t _maxCacheSize = 100 * 1024 * 1024; // Example: 100MB max cache size
        std::mutex _cacheMutex;

        const size_t _entryOverhead = sizeof(std::array<uint8_t, 32>) // Key size
                               + sizeof(std::chrono::steady_clock::time_point) // Timestamp size
                               + 16; // Estimated additional overhead (bucketing, etc.)


        // Generates a unique key based on method and params
        std::array<uint8_t, 32> generateKey(const std::string& method, const Json::Value& params);

        // Removes the oldest entries based on timestamp
        void removeOldestEntries();

    public:
        Cache() = default;

        // Look up a cached response for (method, params). If found, copies it
        // into `response` and returns true; otherwise returns false.
        bool isCached(const std::string& method, const Json::Value& params, Response& response);

        // Insert or refresh the cached response for (method, params), updating
        // its timestamp, then evict oldest entries if over the size budget.
        void addResponse(const std::string& method, const Json::Value& params, const Response& response);

        // Invalidate any cached responses whose validity depends on the given
        // address having changed; removes affected entries and reclaims size.
        void addressChanged(const std::string& address);

        // Invalidate cached responses that expire when a new block is added.
        void newBlockAdded();

        // Invalidate cached responses that expire when a new asset is issued.
        void newAssetIssued();
    };
}



#endif //DIGIASSET_CORE_CACHE_H
