//
// Created by mctrivia on 17/03/24.
//
// RPC method "sendmany".
// Node-side JSON-RPC handler that mirrors DigiByte Core's "sendmany" but adds
// DigiByte Domain support: any recipient key that is a domain is resolved to
// its address (amounts merged if the resolved address already appears) before
// the modified params are forwarded to DigiByte Core's wallet. Result is passed
// straight back to the caller. Dispatched by the node's RPC server.

#include "AppMain.h"
#include "DigiByteDomain.h"
#include "RPC/Response.h"
#include "RPC/Server.h"
#include <cctype>
#include <cstdio>
#include <jsoncpp/json/value.h>
#include <string>

namespace RPC {
    namespace Methods {
        namespace {
            // Convert a DGB amount (JSON number OR string - Core accepts both) to
            // satoshis exactly, with no floating-point rounding. Needed when we
            // merge two outputs that resolve to the same address: summing them as
            // doubles could yield e.g. 0.1+0.2 = 0.30000000000000004, which Core's
            // sendmany rejects as "Invalid amount", and calling asDouble() on a
            // string amount throws - both would break an otherwise valid send.
            int64_t dgbToSats(const Json::Value& v) {
                std::string s;
                if (v.isString() || v.isIntegral()) s = v.asString();
                else if (v.isDouble()) { char b[32]; std::snprintf(b, sizeof(b), "%.8f", v.asDouble()); s = b; }
                else throw DigiByteException(RPC_INVALID_PARAMS, "Invalid amount in sendmany outputs");

                size_t i = 0;
                bool neg = false;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); ++i; }
                std::string whole, frac;
                bool dot = false;
                for (; i < s.size(); ++i) {
                    char c = s[i];
                    if (c == '.') { if (dot) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid amount"); dot = true; }
                    else if (std::isdigit((unsigned char) c)) (dot ? frac : whole).push_back(c);
                    else throw DigiByteException(RPC_INVALID_PARAMS, "Invalid amount");
                }
                if (frac.size() > 8) throw DigiByteException(RPC_INVALID_PARAMS, "Amount has more than 8 decimals");
                while (frac.size() < 8) frac.push_back('0');
                int64_t sats;
                try { sats = (whole.empty() ? 0 : std::stoll(whole)) * 100000000 + (frac.empty() ? 0 : std::stoll(frac)); }
                catch (...) { throw DigiByteException(RPC_INVALID_PARAMS, "Amount out of range"); }
                return neg ? -sats : sats;
            }
            std::string satsToDgb(int64_t sats) {
                bool neg = sats < 0;
                uint64_t a = neg ? (uint64_t)(-(sats)) : (uint64_t)sats;
                char b[32];
                std::snprintf(b, sizeof(b), "%llu.%08llu",
                              (unsigned long long)(a / 100000000ULL), (unsigned long long)(a % 100000000ULL));
                return neg ? ("-" + std::string(b)) : std::string(b);
            }
        }
        /**
        * params - see https://developer.bitcoin.org/reference/rpc/sendmany.html
        * only difference is we now accept domains
        */
        extern const Response sendmany(const Json::Value& params) {
            if (params.size() < 2 || params.size() > 9) {
                throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");
            }
            if (!params[1].isObject()) throw DigiByteException(RPC_INVALID_PARAMS, "Invalid params");

            //check if any domains in outputs
            std::vector<std::string> keysToRemove;
            Value newParams = params;
            for (auto it = newParams[1].begin(); it != newParams[1].end(); ++it) {
                std::string key = it.name();
                Json::Value value = *it;

                if (DigiByteDomain::isDomain(key)) {
                    //change the domain into an address
                    string newKey = DigiByteDomain::getAddress(key);
                    if (newParams[1].isMember(newKey)) {
                        // Merge exactly in satoshis (no double rounding, handles
                        // string amounts) and emit a valid 8-decimal amount string.
                        int64_t merged = dgbToSats(newParams[1][newKey]) + dgbToSats(value);
                        newParams[1][newKey] = satsToDgb(merged);
                    } else {
                        newParams[1][newKey] = value;
                    }

                    // Mark the old key for removal
                    keysToRemove.push_back(key);
                }
            }

            // Remove the old keys
            for (const auto& key: keysToRemove) {
                newParams[1].removeMember(key);
            }

            //send modified params to wallet
            Json::Value result=AppMain::GetInstance()->getDigiByteCore()->sendcommand("sendmany", newParams);

            //return response
            Response response;
            response.setResult(result);
            response.setBlocksGoodFor(-1); //do not cache
            return response;
        }

    }
}