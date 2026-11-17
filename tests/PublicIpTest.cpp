//
// publicip= in config.cfg: the IP a node is published at when its outbound traffic leaves
// from another one.  Every "our public IP" use goes through IPFS::getIP, so these check the
// override, its validation, and that the address sent to the pool is then the published one.
//

#include "IPFS.h"
#include "gtest/gtest.h"

using namespace std;

namespace {
    // Each test leaves the setting cleared so no other test sees it.
    struct PublicIpTest: public ::testing::Test {
        void TearDown() override { IPFS::setPublicIp(""); }
    };
}

TEST_F(PublicIpTest, unsetByDefault) {
    EXPECT_TRUE(IPFS::configuredPublicIp().empty());
}

TEST_F(PublicIpTest, configuredValueIsWhatGetIpReturns) {
    ASSERT_TRUE(IPFS::setPublicIp("203.0.113.10"));
    EXPECT_EQ(IPFS::configuredPublicIp(), "203.0.113.10");
    EXPECT_EQ(IPFS::getIP(), "203.0.113.10");   //no lookup: the configured value wins
}

TEST_F(PublicIpTest, trimsSpacesAndQuotes) {
    ASSERT_TRUE(IPFS::setPublicIp("  \"203.0.113.10\" "));
    EXPECT_EQ(IPFS::configuredPublicIp(), "203.0.113.10");
}

TEST_F(PublicIpTest, acceptsIpv6) {
    ASSERT_TRUE(IPFS::setPublicIp("2001:db8::10"));
    EXPECT_EQ(IPFS::configuredPublicIp(), "2001:db8::10");
}

TEST_F(PublicIpTest, rejectsWhatIsNotAnAddressAndKeepsTheOldValue) {
    ASSERT_TRUE(IPFS::setPublicIp("203.0.113.10"));
    EXPECT_FALSE(IPFS::setPublicIp("my.host.example"));
    EXPECT_FALSE(IPFS::setPublicIp("203.0.113.10 # office"));
    EXPECT_FALSE(IPFS::setPublicIp("abc"));
    EXPECT_EQ(IPFS::configuredPublicIp(), "203.0.113.10");
}

TEST_F(PublicIpTest, emptyClearsIt) {
    ASSERT_TRUE(IPFS::setPublicIp("203.0.113.10"));
    ASSERT_TRUE(IPFS::setPublicIp(""));
    EXPECT_TRUE(IPFS::configuredPublicIp().empty());
}

// The case that went wrong: IPFS announces the published IP, outbound traffic leaves from
// another.  With the outbound IP the pool was handed a made-up address on it; with publicip
// it gets the real announced one.
TEST_F(PublicIpTest, poolGetsTheAnnouncedAddressForThePublishedIp) {
    const string self = "12D3KooWExampleSelfPeerId";
    vector<string> addrs = {
            "/ip4/192.168.1.20/tcp/4001/p2p/" + self,
            "/ip4/203.0.113.10/tcp/4001/p2p/" + self};
    EXPECT_EQ(IPFS::findPublicAddress(addrs, "203.0.113.10"), "/ip4/203.0.113.10/tcp/4001/p2p/" + self);
    EXPECT_EQ(IPFS::findPublicAddress(addrs, "198.51.100.7"), "/ip4/198.51.100.7/tcp/4001/p2p/" + self);
}

// ---- spotting a second public IP in IPFS's /id (the case where [F] picked the wrong one) ----

#include "ConsoleDashboard.h"

TEST(OtherPublicIpv4s, findsAPublicAddressThatIsNotOurs) {
    string id = R"({"ID":"12D3KooWSelf","Addresses":["/ip4/127.0.0.1/tcp/4001/p2p/12D3KooWSelf",)"
                R"("/ip4/192.168.1.20/tcp/4001/p2p/12D3KooWSelf","/ip4/203.0.113.10/tcp/4001/p2p/12D3KooWSelf",)"
                R"("/ip4/203.0.113.10/udp/4001/quic-v1/p2p/12D3KooWSelf","/ip4/198.51.100.7/tcp/4001/p2p/12D3KooWSelf"]})";
    auto other = ConsoleDashboard::otherPublicIpv4s(id, "198.51.100.7");
    ASSERT_EQ(other.size(), 1u);                 //listed once although it has two transports
    EXPECT_EQ(other[0], "203.0.113.10");
}

TEST(OtherPublicIpv4s, singleIpBoxHasNone) {
    string id = R"({"Addresses":["/ip4/10.0.0.5/tcp/4001/p2p/S","/ip4/172.20.1.1/tcp/4001/p2p/S",)"
                R"("/ip4/100.64.3.2/tcp/4001/p2p/S","/ip4/198.51.100.7/tcp/4001/p2p/S"]})";
    EXPECT_TRUE(ConsoleDashboard::otherPublicIpv4s(id, "198.51.100.7").empty());
}

TEST(OtherPublicIpv4s, relayAddressesAreNotWhereWeAreSeen) {
    string id = R"({"Addresses":["/ip4/203.0.113.99/tcp/4001/p2p/12D3KooWRelay/p2p-circuit/p2p/S"]})";
    EXPECT_TRUE(ConsoleDashboard::otherPublicIpv4s(id, "198.51.100.7").empty());
}
