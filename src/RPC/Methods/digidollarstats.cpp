//
// Created by mctrivia on 16/08/26.
//

#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include "utils.h"
#include <jsoncpp/json/value.h>
#include <limits>

namespace RPC {
    namespace Methods {
        /**
         * Returns DigiDollar supply, collateral and oracle price over time.
         * Warning: the last result is likely not a full time period.
         *
         * The series begins at the first time window that ends at or after the DigiDollar
         * activation height(23,869,440), so it is short compared to algostats and addressstats.
         * Windows before activation are omitted rather than returned as zeros.
         *
         * Input Parameters:
         *  params[0] - start time (integer, default = beginning(0))
         *  params[1] - end time (integer, default = end(max value))
         *  params[2] - time frame (integer, default = day(86400))
         *
         * Output Format:
         *  JSON array of objects, each containing:
         *    - "time": end time of the time frame (integer)
         *    - "supply": DigiDollar in circulation at window end, in cents (integer)
         *    - "supplyAmount": the same value as decimal dollars (string)
         *    - "collateral": DGB sats locked in unredeemed vaults at window end (integer)
         *    - "collateralAmount": the same value in decimal DGB (string)
         *    - "vaults": unredeemed vault count at window end (integer)
         *    - "holders": addresses holding DigiDollar at window end (integer)
         *    - "minted": cents minted during the window (integer)
         *    - "redeemed": cents released by redemptions during the window (integer)
         *    - "mints": mint transaction count during the window (integer)
         *    - "redemptions": redemption count during the window (integer)
         *    - "transfers": transfer count during the window (integer)
         *    - "price": object with "min", "max" and "avg" in micro USD per DGB.  Omitted for a
         *               window in which no oracle commitment was recorded.
         *    - "ratio": collateral value divided by circulating value (double).  Omitted when
         *               either the price or the supply is unknown for that window.
         */
        extern const Response digidollarstats(const Json::Value& params) {
            //get parameters
            unsigned int timeFrame = 86400;
            unsigned int start = 0;
            unsigned int end = std::numeric_limits<unsigned int>::max();
            if (params.size() > 3) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            if (params.size() >= 1) {
                if (!params[0].isInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                start = params[0].asInt();
            }
            if (params.size() >= 2) {
                if (!params[1].isInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                end = params[1].asInt();
            }
            if (params.size() == 3) {
                if (!params[2].isInt()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
                timeFrame = params[2].asInt();
            }
            if (end < start) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");

            Database* db = AppMain::GetInstance()->getDatabase();
            std::vector<DigiDollarStats> stats = db->getDigiDollarStats(start, end, timeFrame);

            Value result = Json::arrayValue;
            for (const DigiDollarStats& stat: stats) {
                Value entry = Json::objectValue;
                entry["time"] = stat.time;
                entry["supply"] = static_cast<Json::UInt64>(stat.supply);
                entry["supplyAmount"] = utils::toDecimalString(stat.supply, 2);
                entry["collateral"] = static_cast<Json::UInt64>(stat.collateral);
                entry["collateralAmount"] = utils::toDecimalString(stat.collateral, 8);
                entry["vaults"] = stat.vaults;
                entry["holders"] = stat.holders;
                entry["minted"] = static_cast<Json::UInt64>(stat.minted);
                entry["redeemed"] = static_cast<Json::UInt64>(stat.redeemed);
                entry["mints"] = stat.mints;
                entry["redemptions"] = stat.redemptions;
                entry["transfers"] = stat.transfers;

                //price columns are NULL for a window with no commitment, which surfaces as 0 here
                if (stat.priceMax > 0) {
                    Value price = Json::objectValue;
                    price["min"] = static_cast<Json::UInt64>(stat.priceMin);
                    price["max"] = static_cast<Json::UInt64>(stat.priceMax);
                    price["avg"] = stat.priceAvg;
                    entry["price"] = price;

                    if ((stat.supply > 0) && (stat.priceAvg > 0)) {
                        //collateral sats to cents at the window's average price, same maths as
                        //getdigidollarinfo: cents = sats/1e8 * price/1e6 * 100
                        double collateralCents = static_cast<double>(stat.collateral) * stat.priceAvg / 1e12;
                        entry["ratio"] = collateralCents / static_cast<double>(stat.supply);
                    }
                }
                result.append(entry);
            }

            Response response;
            response.setResult(result);
            response.setBlocksGoodFor(5760); //day
            return response;
        }

    }
}
