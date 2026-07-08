//
// Created by mctrivia on 04/04/24.
//
// Implements RPC::Response (see Response.h): the payload + cache-lifetime
// metadata every JSON-RPC method returns. Each mutator that stores data also
// adds that data's estimated byte size to _size so the RPC server's response
// cache can track and bound its total memory use.
//

#include "Response.h"
#include "utils.h"
#include <algorithm>

namespace RPC {
    //Store a success payload. Clears the error flag and grows _size by the
    //estimated memory footprint of the stored value.
    void Response::setResult(const Json::Value& result) {
        _result = result;
        _error = false;
        _size += utils::estimateJsonMemoryUsage(result);
    }
    //Store an error payload in the same slot as the result and mark this
    //Response as an error; toJSON() will emit it under "error". Grows _size.
    void Response::setError(const Json::Value& error) {
        _result = error;
        _error = true;
        _size += utils::estimateJsonMemoryUsage(error);
    }
    //Register an address whose modification should invalidate this cached
    //response. De-duplicates: only adds (and only grows _size) if not present.
    void Response::addInvalidateOnAddressChange(const std::string& address) {
        // Check if the address already exists in the vector
        if (std::find(_invalidateOnAddressChange.begin(), _invalidateOnAddressChange.end(), address) == _invalidateOnAddressChange.end()) {
            // Address not found, so add it to the vector
            _invalidateOnAddressChange.emplace_back(address);
            _size += address.size();
        }
    }
    void Response::setBlocksGoodFor(int blocks) {
        _blocksGoodFor = blocks;
    }
    void Response::setInvalidateOnNewAsset() {
        _invalidateOnNewAsset=true;
    }
    //True when nothing has been stored yet: _size is still just the base object
    //size, meaning no result/error or invalidation data was ever added.
    bool Response::empty() const {
        return (_size == sizeof(Response));
    }
    //A response marked blocksGoodFor<0 must NEVER be cached. Eviction otherwise
    //relies on newBlockAdded(), which the analyzer skips during bulk sync - so a
    //"never cache" (or blocksGoodFor==0) live response would be served stale for
    //the whole initial sync. The cache consults this before storing.
    bool Response::isCacheable() const {
        return _blocksGoodFor >= 0;
    }
    //Cache-eviction predicate: if the given address is in this response's
    //watch list, return the response's size (signal to delete); otherwise 0.
    size_t Response::addressChanged(const std::string& address) const {
        // Check if the address is in the _invalidateOnAddressChange vector
        auto it = std::find(_invalidateOnAddressChange.begin(), _invalidateOnAddressChange.end(), address);

        // If the address is found, return the size of the Response object
        if (it != _invalidateOnAddressChange.end()) {
            return _size;
        }

        // If the address is not found, return 0
        return 0;
    }
    //Cache-eviction predicate, called once per new block: decrements the
    //remaining block budget and, once it drops below 0, returns the response's
    //size (signal to delete). A response set with blocksGoodFor < 0 expires on
    //the first block. Mutates _blocksGoodFor.
    size_t Response::newBlockAdded() {
        _blocksGoodFor--;
        if (_blocksGoodFor<0) return _size;
        return 0;
    }
    //Cache-eviction predicate: returns the response's size (signal to delete)
    //only if this response opted in via setInvalidateOnNewAsset(); else 0.
    size_t Response::newAssetIssued() {
        if (!_invalidateOnNewAsset) return 0;
        return _size;
    }
    size_t Response::size() const {
        return _size;
    }

    //Serialize into the JSON-RPC reply envelope for the given request id.
    //On error, "result" is null and "error" carries the payload; on success the
    //reverse. Always sets "id".
    Json::Value Response::toJSON(const Json::Value& id) const {
        Json::Value json =Json::objectValue;
        if (_error) {
            json["result"]=Json::nullValue;
            json["error"]=_result;
        } else {
            json["result"]=_result;
            json["error"]=Json::nullValue;
        }
        json["id"]=id;
        return json;
    }
}