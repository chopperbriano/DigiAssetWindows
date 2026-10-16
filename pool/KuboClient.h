//
// KuboClient - the two IPFS calls the storage plan needs from the pool's own
// node: how big a file is, and a byte range of it. Header-only over
// CurlHandler, like the verifier's other kubo calls.
//

#ifndef DIGIASSET_POOL_KUBOCLIENT_H
#define DIGIASSET_POOL_KUBOCLIENT_H

#include "CurlHandler.h"
#include <cstdint>
#include <string>

namespace KuboClient {

    // A CID is alphanumeric throughout (base58btc or base32). Anything else is
    // refused before it reaches a URL.
    inline bool safeCid(const std::string& cid) {
        if (cid.empty() || cid.size() > 128) return false;
        for (unsigned char c: cid) {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
        }
        return true;
    }

    // The file's size in bytes, from files/stat on /ipfs/<cid>. "Size" is the
    // file's own length (not the DAG's cumulative size), which is what a byte
    // range is measured against. False if kubo cannot answer in time.
    inline bool fileSize(const std::string& apiBase, const std::string& cid, int64_t& out,
                         unsigned int timeoutMs = 15000) {
        if (!safeCid(cid)) return false;
        try {
            std::string r = CurlHandler::post(apiBase + "files/stat?arg=/ipfs/" + cid, {}, timeoutMs);
            size_t p = r.find("\"Size\":");
            if (p == std::string::npos) return false;
            p += 7;
            int64_t v = 0;
            bool any = false;
            while (p < r.size() && r[p] >= '0' && r[p] <= '9') {
                v = v * 10 + (r[p] - '0');
                p++;
                any = true;
            }
            if (!any) return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    }

    // Bytes [offset, offset+length) of the file. False on any failure, or if
    // kubo returned a different number of bytes than asked for.
    inline bool catRange(const std::string& apiBase, const std::string& cid, int64_t offset, int64_t length,
                         std::string& out, unsigned int timeoutMs = 60000) {
        if (!safeCid(cid) || offset < 0 || length <= 0) return false;
        try {
            out = CurlHandler::post(apiBase + "cat?arg=" + cid + "&offset=" + std::to_string(offset) +
                                            "&length=" + std::to_string(length),
                                    {}, timeoutMs);
            return (int64_t) out.size() == length;
        } catch (...) {
            return false;
        }
    }

} // namespace KuboClient

#endif // DIGIASSET_POOL_KUBOCLIENT_H
