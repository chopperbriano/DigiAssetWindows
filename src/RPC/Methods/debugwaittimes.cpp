//
// Created by mctrivia on 21/03/24.
//
// RPC method handler: "debugwaittimes".
//
// Part of the node's JSON-RPC surface. A diagnostic/profiling method that
// collects wait-time and profiling info from the Database, DigiByte Core
// connection, and Chain Analyzer, and returns it as a JSON array of text lines
// to help identify performance bottlenecks.
//
#include "AppMain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <jsoncpp/json/value.h>

namespace RPC {
    namespace Methods {
        /**
        * Returns a string of wait times to help improve performance
        */
        extern const Response debugwaittimes(const Json::Value& params) {
            std::string result;
            result += AppMain::GetInstance()->getDatabase()->printProfilingInfo();
            result += AppMain::GetInstance()->getDigiByteCore()->printProfilingInfo();
            result += AppMain::GetInstance()->getChainAnalyzer()->printProfilingInfo();

            std::istringstream stream(result);
            std::string line;
            Json::Value jsonArray(Json::arrayValue);
            while (std::getline(stream, line)) {
                // If you want to exclude empty lines, uncomment the following line
                // if (line.empty()) continue;
                jsonArray.append(line);
            }

            //return response
            Response response;
            response.setResult(jsonArray);
            response.setBlocksGoodFor(-1);
            return response;
        }

    } // namespace Methods
} // namespace RPC