//
// getnodestats - returns a snapshot of this node's current operational state
// so two nodes can be compared side-by-side to verify they're processing the
// same chain and serving the same DigiAsset content.
//
// Fields:
//   buildVersion       - e.g. "0.3.0-win.31"
//   syncHeight         - chain analyzer's current block height
//   assetCount         - distinct assetIds in local database (SELECT
//                        COUNT(DISTINCT assetId) FROM assets). This
//                        undercounts when the same assetId has been
//                        reissued N times -- see topAssetIndex for the
//                        pagination-correct number.
//   topAssetIndex      - highest assetIndex assigned (MAX(assetIndex)
//                        FROM assets). This is what listlastassets /
//                        listassets are keyed on, so paginating
//                        explorers should use this as the total.
//   bitswap:
//     available        - whether IPFS API was reachable on the last poll
//     blocksSent       - monotonic count of blocks served out via bitswap
//     dataSent         - bytes served
//     blocksPerMin     - outbound rate from last 30-second window
//   permanentCoverage:
//     checked          - whether the coverage scan has completed at least once
//     tracked          - number of assetIds mctrivia's permanent list tracks
//     have             - of those, how many are in our local assets table
//     missingCount     - tracked - have
//
// Usage:
//   digiasset_core-cli getnodestats
//
// Run the same command on two different nodes; if assetCount differs at the
// same syncHeight, one of them has a chain-analyzer bug.
//

#include "AppMain.h"
#include "NodeStats.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include "Version.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Builds a JSON snapshot of this node's operational state (see file
        * header for the full field list): build version, chain-analyzer sync
        * height, local asset counts, and the bitswap / permanent-coverage
        * figures cached in NodeStats by the dashboard's background pollers.
        * Takes no params. Missing subsystems (analyzer/db not yet set, or
        * NodeStats not yet polled) report zero/"not probed" rather than
        * erroring. Response is marked non-cacheable so operators can poll it.
        */
        extern const Response getnodestats(const Json::Value& /*params*/) {
            Json::Value result;
            result["buildVersion"] = getVersionString();

            // Sync height - pull directly from chain analyzer if available.
            ChainAnalyzer* analyzer = AppMain::GetInstance()->getChainAnalyzerIfSet();
            result["syncHeight"] = analyzer ? (Json::UInt)analyzer->getSyncHeight() : 0;

            // Local asset count - pulled directly from db. "assetCount"
            // keeps its distinct-assetId semantics for backward compat;
            // "topAssetIndex" is the pagination-correct figure.
            Database* db = AppMain::GetInstance()->getDatabaseIfSet();
            result["assetCount"] = db ? (Json::UInt64)db->getAssetCountOnChain() : 0;
            result["topAssetIndex"] = db ? (Json::UInt64)db->getMaxAssetIndex() : 0;

            // Bitswap + coverage come from NodeStats, which the dashboard's
            // background pollers populate. If the dashboard hasn't run a
            // poll yet (fresh process), these fields will reflect the
            // "not yet probed" state instead of blocking the RPC caller.
            auto snap = NodeStats::instance().snapshot();

            Json::Value bitswap;
            bitswap["probed"] = snap.bitswapProbed;
            bitswap["available"] = snap.bitswapAvailable;
            bitswap["blocksSent"] = (Json::UInt64)snap.blocksSent;
            bitswap["dataSent"] = (Json::UInt64)snap.dataSent;
            bitswap["blocksPerMin"] = snap.blocksPerMin;
            result["bitswap"] = bitswap;

            Json::Value coverage;
            coverage["checked"] = snap.coverageChecked;
            coverage["tracked"] = (Json::UInt)snap.coverageTracked;
            coverage["have"] = (Json::UInt)snap.coverageHave;
            coverage["missingCount"] = (Json::UInt)(snap.coverageTracked > snap.coverageHave
                                                            ? snap.coverageTracked - snap.coverageHave
                                                            : 0);
            result["permanentCoverage"] = coverage;

            Response response;
            response.setResult(result);
            // Never cache - operators poll this to watch live sync progress. Must
            // be <0 (not 0): a 0 response is still cached and only evicted by
            // newBlockAdded(), which is skipped during bulk sync, so it would
            // report the same syncHeight for the whole initial sync.
            response.setBlocksGoodFor(-1);
            return response;
        }
    }
}
