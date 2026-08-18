//
// Created by mctrivia on 16/08/26.
//

#include "AppMain.h"
#include "DigiAssetConstants.h"
#include "DigiDollar.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include "utils.h"
#include <jsoncpp/json/value.h>
#include <stdexcept>

namespace RPC {
    namespace Methods {
        /**
        * Returns the current state of the DigiDollar system.
        *
        * params[0] - optional block height.  If provided the oracle price returned is the one that
        *             was current at that height.  Supply and collateral figures are always current
        *             because they are not stored per height.
        *
        * @return object:
        *   "active"(bool) - whether DigiDollar has activated on the chain we have synced
        *   "activationHeight"(integer) - 23869440 on mainnet
        *   "price"(object) - latest oracle price, absent if none has been recorded yet:
        *       "microUSDPerDGB"(integer) - the raw published value.  4216 means $0.004216 per DGB
        *       "usdPerDGB"(double) - the same value as a decimal, for display only
        *       "satsPerUSD"(integer) - DGB sats one USD is worth, the same convention the
        *                               exchange rate table uses(see getexchangerates)
        *       "height"(integer) - block the commitment was first seen in
        *       "epoch"(integer) - oracle epoch, which is height/40
        *       "time"(integer) - unix time the oracles sampled the price, not the block time
        *       "oracles"(integer) - how many of the 35 oracle slots signed this price
        *   "supply"(object):
        *       "cents"(integer) - DigiDollar in circulation, in cents
        *       "amount"(string) - the same value as decimal dollars
        *       "holders"(integer) - addresses holding a non zero balance
        *   "collateral"(object):
        *       "sats"(integer) - DGB locked in unredeemed vaults
        *       "amount"(string) - the same value in decimal DGB
        *       "vaults"(integer) - unredeemed vault count
        *       "ratio"(double) - collateral value divided by circulating value.  Only present when
        *                         an oracle price is known, since it cannot be computed without one.
        *   "lifetime"(object):
        *       "mintedCents"(integer) - total ever minted
        *       "redeemedCents"(integer) - total released by redemptions
        *   "indexed"(bool) - false when this node has not finished indexing DigiDollar history, in
        *                     which case every figure above is incomplete
        */
        extern const Response getdigidollarinfo(const Json::Value& params) {
            AppMain* main = AppMain::GetInstance();
            if (params.size() > 1) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");

            Database* db = main->getDatabase();
            ChainAnalyzer* analyzer = main->getChainAnalyzer();

            unsigned int height = analyzer->getSyncHeight();
            bool heightProvided = false;
            if (params.size() == 1) {
                if (!params[0].isUInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                height = params[0].asUInt();
                if (height > analyzer->getSyncHeight()) throw DigiByteException(RPC_MISC_ERROR, "Height out of range");
                heightProvided = true;
            }

            Value result = Json::objectValue;
            result["activationHeight"] = DigiAssetConstants::DIGIDOLLAR_ACTIVATION_HEIGHT;
            result["active"] = (analyzer->getSyncHeight() >= DigiAssetConstants::DIGIDOLLAR_ACTIVATION_HEIGHT);
            result["indexed"] = (db->getDigiDollarSyncHeight() >= DigiAssetConstants::DIGIDOLLAR_ACTIVATION_HEIGHT);

            DigiDollarSummary summary = db->getDigiDollarSummary();

            //price, either latest or as at the requested height
            uint64_t price = 0;
            try {
                DigiDollarRate rate = heightProvided ? db->getDigiDollarRateAtHeight(height)
                                                     : db->getCurrentDigiDollarRate();
                price = rate.price;
                Value priceObj = Json::objectValue;
                priceObj["microUSDPerDGB"] = static_cast<Json::UInt64>(rate.price);
                priceObj["usdPerDGB"] = static_cast<double>(rate.price) / 1e6;
                priceObj["satsPerUSD"] = static_cast<Json::UInt64>(DigiDollar::priceToExchangeRate(rate.price));
                priceObj["height"] = rate.height;
                priceObj["epoch"] = rate.epoch;
                priceObj["time"] = rate.time;
                priceObj["oracles"] = rate.participants;
                result["price"] = priceObj;
            } catch (const std::out_of_range& e) {
                //no oracle commitment recorded yet - leave "price" absent rather than faking a zero
            }

            Value supply = Json::objectValue;
            supply["cents"] = static_cast<Json::UInt64>(summary.supply);
            supply["amount"] = utils::toDecimalString(summary.supply, 2);
            supply["holders"] = summary.holders;
            result["supply"] = supply;

            Value collateral = Json::objectValue;
            collateral["sats"] = static_cast<Json::UInt64>(summary.collateral);
            collateral["amount"] = utils::toDecimalString(summary.collateral, 8);
            collateral["vaults"] = summary.vaults;
            if ((price > 0) && (summary.supply > 0)) {
                //collateral is DGB sats, supply is cents.  Convert the collateral to cents using the
                //oracle price so the ratio is dimensionless:
                //  cents = sats/1e8 * price/1e6 * 100
                double collateralCents = static_cast<double>(summary.collateral) *
                                         static_cast<double>(price) / 1e12;
                collateral["ratio"] = collateralCents / static_cast<double>(summary.supply);
            }
            result["collateral"] = collateral;

            Value lifetime = Json::objectValue;
            lifetime["mintedCents"] = static_cast<Json::UInt64>(summary.mintedTotal);
            lifetime["redeemedCents"] = static_cast<Json::UInt64>(summary.redeemedTotal);
            result["lifetime"] = lifetime;

            Response response;
            response.setResult(result);
            //a new oracle commitment lands roughly every 40 blocks, so do not cache past one epoch
            response.setBlocksGoodFor(DigiAssetConstants::DIGIDOLLAR_ORACLE_EPOCH_LENGTH);
            return response;
        }

    }
}
