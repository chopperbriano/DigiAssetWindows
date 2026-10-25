//
// Tests for pool/CoreRpc.h - which DigiByte Core wallet the pool's RPC calls (payouts above all)
// are sent to.  Header only, no node or DigiByte Core needed.
//

#include "gtest/gtest.h"
#include "../pool/CoreRpc.h"

TEST(CoreRpc, WalletNameReadsWithOrWithoutQuotes) {
    EXPECT_EQ(CoreRpc::parseWalletName("DigiAssetWindows-Wallet"), "DigiAssetWindows-Wallet");
    EXPECT_EQ(CoreRpc::parseWalletName("\"DigiAssetWindows-Wallet\""), "DigiAssetWindows-Wallet");
    EXPECT_EQ(CoreRpc::parseWalletName("  \"DigiAssetWindows-Wallet\" "), "DigiAssetWindows-Wallet");
    EXPECT_EQ(CoreRpc::parseWalletName("\"\""), "");   //Core's unnamed default wallet
}

TEST(CoreRpc, UrlNamesTheWalletOnlyWhenConfigured) {
    CoreRpc::configure({});
    EXPECT_FALSE(CoreRpc::walletNamed());
    EXPECT_EQ(CoreRpc::url("u", "p", 14022), "http://u:p@127.0.0.1:14022");

    CoreRpc::configure({{"poolrpcwallet", "\"DigiAssetWindows-Wallet\""}});
    EXPECT_TRUE(CoreRpc::walletNamed());
    EXPECT_EQ(CoreRpc::url("u", "p", 14022), "http://u:p@127.0.0.1:14022/wallet/DigiAssetWindows-Wallet");

    CoreRpc::configure({{"poolrpcwallet", "\"\""}});   //named, and the name is ""
    EXPECT_TRUE(CoreRpc::walletNamed());
    EXPECT_EQ(CoreRpc::url("u", "p", 14022), "http://u:p@127.0.0.1:14022/wallet/");

    CoreRpc::configure({{"poolrpcwallet", "my wallet"}});   //percent-encoded like digibyte-cli
    EXPECT_EQ(CoreRpc::url("u", "p", 14022), "http://u:p@127.0.0.1:14022/wallet/my%20wallet");
    CoreRpc::configure({});
}
