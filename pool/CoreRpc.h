//
// Which DigiByte Core wallet the pool talks to.
//
// Core answers wallet commands on the plain RPC endpoint only while exactly one wallet is
// loaded; with two or more, every one of them fails ("Wallet file not specified").  Worse,
// with exactly one OTHER wallet loaded - say only another app's, after a restart - an
// unnamed call quietly uses that one, and a payout would spend the wrong wallet's coins.
// So the pool names its wallet: poolrpcwallet in pool.cfg, sent as /wallet/<name> the way
// digibyte-cli -rpcwallet does.  Payouts refuse to run without it (PoolDashboard).
//
// poolrpcwallet=DigiAssetWindows-Wallet and poolrpcwallet="DigiAssetWindows-Wallet" mean the
// same wallet; poolrpcwallet="" is Core's unnamed default wallet.
//

#ifndef DIGIASSET_POOL_COREPC_H
#define DIGIASSET_POOL_COREPC_H

#include <cctype>
#include <map>
#include <mutex>
#include <string>

namespace CoreRpc {

    namespace detail {
        struct Choice {
            bool named = false;
            std::string name;
        };
        inline std::mutex& lock() {
            static std::mutex* m = new std::mutex; //leaked: used by threads that outlive statics
            return *m;
        }
        inline Choice& choice() {
            static Choice* c = new Choice;
            return *c;
        }
        inline std::string encode(const std::string& in) {
            static const char* hex = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c: in) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    out += (char) c;
                } else {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
            }
            return out;
        }
    }

    // Reads a poolrpcwallet value: surrounding spaces trimmed, one pair of surrounding quotes
    // removed.  Pure, so it can be tested.
    inline std::string parseWalletName(const std::string& raw) {
        size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        size_t b = raw.find_last_not_of(" \t");
        std::string v = raw.substr(a, b - a + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }

    // Takes poolrpcwallet from a parsed pool.cfg.  Called at startup and again whenever the
    // dashboard re-reads pool.cfg, so an edit takes effect at the next payout.
    inline void configure(const std::map<std::string, std::string>& cfg) {
        std::lock_guard<std::mutex> l(detail::lock());
        auto it = cfg.find("poolrpcwallet");
        detail::choice().named = (it != cfg.end());
        detail::choice().name = (it != cfg.end()) ? parseWalletName(it->second) : "";
    }

    inline bool walletNamed() {
        std::lock_guard<std::mutex> l(detail::lock());
        return detail::choice().named;
    }

    inline std::string walletName() {
        std::lock_guard<std::mutex> l(detail::lock());
        return detail::choice().name;
    }

    // RPC url for this pool's DigiByte Core, pointed at the named wallet when there is one.
    // /wallet/<name> serves every RPC, wallet or not, so it is safe for all calls.
    inline std::string url(const std::string& user, const std::string& pass, int port) {
        std::string base = "http://" + user + ":" + pass + "@127.0.0.1:" + std::to_string(port);
        std::lock_guard<std::mutex> l(detail::lock());
        if (!detail::choice().named) return base;
        return base + "/wallet/" + detail::encode(detail::choice().name);
    }
}

#endif //DIGIASSET_POOL_COREPC_H
