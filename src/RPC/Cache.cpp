//
// Created by mctrivia on 05/04/24.
//
// RPC::Cache implementation - see Cache.h. Provides the response cache used by
// the node's JSON-RPC server: key generation, thread-safe lookup/insert,
// event-driven invalidation, and size-based eviction.
//

#include "Cache.h"
#include "crypto/SHA256.h"

namespace RPC {

    // Build the cache key by hashing the method name concatenated with the
    // styled-JSON serialization of its params, so identical requests collide
    // to the same 32-byte SHA256 key.
    std::array<uint8_t, 32> Cache::generateKey(const std::string& method, const Json::Value& params) {
        // Convert method and params to a single string
        std::string input = method + params.toStyledString();

        // Initialize SHA256 object and update it with the input
        SHA256 sha256;
        sha256.update(input);

        // Compute the digest
        return sha256.digest();
    }

    // Under lock, find the entry for (method, params); on hit copy the stored
    // Response out and return true, else return false. Does not refresh age.
    bool Cache::isCached(const std::string& method, const Json::Value& params, Response& response) {
        std::array<uint8_t, 32> key = generateKey(method, params);
        std::lock_guard<std::mutex> lock(_cacheMutex);
        auto it = _cacheMap.find(key);
        if (it == _cacheMap.end()) {
            return false;
        }
        response = it->second.response;
        return true;
    }

    // Under lock, insert a new entry or overwrite an existing one and reset its
    // timestamp. New entries add their byte size (plus per-entry overhead) to
    // the running total; then removeOldestEntries() enforces the size cap.
    // Note: overwriting an existing entry does not adjust the size total.
    void Cache::addResponse(const std::string& method, const Json::Value& params, const Response& response) {
        std::array<uint8_t, 32> key = generateKey(method, params);
        std::lock_guard<std::mutex> lock(_cacheMutex);

        // Check if the entry already exists and update it
        auto it = _cacheMap.find(key);
        if (it != _cacheMap.end()) {
            // Update existing entry
            it->second.response = response;
            it->second.timestamp = std::chrono::steady_clock::now();
        } else {
            // Add new entry
            _currentCacheSize += response.size() + _entryOverhead;
            _cacheMap.emplace(key, CacheEntry(response));
        }

        // Ensure cache does not exceed max size
        removeOldestEntries();
    }

    // Under lock, ask each cached Response whether it must be dropped because
    // the given address changed. A non-zero returned size means invalidate:
    // subtract its size and overhead from the total and erase the entry.
    void Cache::addressChanged(const std::string& address) {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        for (auto it = _cacheMap.begin(); it != _cacheMap.end(); ) {
            size_t deleteSize=it->second.response.addressChanged(address);
            if ( deleteSize > 0) {
                _currentCacheSize -= (deleteSize + _entryOverhead);
                it = _cacheMap.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Under lock, drop every cached Response that reports it is no longer valid
    // now that a new block has been added (e.g. its blocksGoodFor has elapsed),
    // reclaiming the freed size from the running total.
    void Cache::newBlockAdded() {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        for (auto it = _cacheMap.begin(); it != _cacheMap.end(); ) {
            size_t deleteSize=it->second.response.newBlockAdded();
            if ( deleteSize > 0) {
                _currentCacheSize -= (deleteSize + _entryOverhead);
                it = _cacheMap.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Under lock, drop every cached Response that reports it is invalidated by
    // a newly issued asset, reclaiming the freed size from the running total.
    void Cache::newAssetIssued() {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        for (auto it = _cacheMap.begin(); it != _cacheMap.end(); ) {
            size_t deleteSize=it->second.response.newAssetIssued();
            if ( deleteSize > 0) {
                _currentCacheSize -= (deleteSize + _entryOverhead);
                it = _cacheMap.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Evict entries oldest-first (by timestamp) until the running size total is
    // back within _maxCacheSize. Called with the mutex already held. O(n) scan
    // per eviction. Caller must hold _cacheMutex.
    void Cache::removeOldestEntries() {
        while (_currentCacheSize > _maxCacheSize && !_cacheMap.empty()) {
            auto oldest = _cacheMap.begin();
            for (auto it = _cacheMap.begin(); it != _cacheMap.end(); ++it) {
                if (it->second.timestamp < oldest->second.timestamp) {
                    oldest = it;
                }
            }
            _currentCacheSize -= (oldest->second.response.size() + _entryOverhead);
            _cacheMap.erase(oldest);
        }
    }
}
