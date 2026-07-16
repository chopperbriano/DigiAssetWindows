//
// Created by mctrivia on 13/06/23.
//

#include "gtest/gtest.h"
#include "IPFS.h"

using namespace std;


TEST(IPFS, sha256ToCID) {
    EXPECT_EQ(IPFS::sha256ToCID("51D3CC662F89E8535D9CF74751DA0F91335A083CF12CB4C9BA81FFF25458274D"),
              "bafkreicr2pggml4j5bjv3hhxi5i5ud4rgnnaqphrfs2mtoub77zfiwbhju");
    BitIO testData = BitIO::makeHexString("51D3CC662F89E8535D9CF74751DA0F91335A083CF12CB4C9BA81FFF25458274D");
    EXPECT_EQ(IPFS::sha256ToCID(testData), "bafkreicr2pggml4j5bjv3hhxi5i5ud4rgnnaqphrfs2mtoub77zfiwbhju");
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helper methods — no daemon required
// ─────────────────────────────────────────────────────────────────────────────

TEST(IPFS, isValidCID) {
    // Known valid CID (alphanumeric only)
    EXPECT_TRUE(IPFS::isValidCID("bafkreicr2pggml4j5bjv3hhxi5i5ud4rgnnaqphrfs2mtoub77zfiwbhju"));
    EXPECT_TRUE(IPFS::isValidCID("QmYwAPJzv5CZsnANOEV4KmUTRMGW9TQYAKnGJkFsaMoZg2"));

    // Empty string is invalid
    EXPECT_FALSE(IPFS::isValidCID(""));

    // Spaces and slashes are invalid
    EXPECT_FALSE(IPFS::isValidCID("has space"));
    EXPECT_FALSE(IPFS::isValidCID("has/slash"));
    EXPECT_FALSE(IPFS::isValidCID("has-dash"));
    EXPECT_FALSE(IPFS::isValidCID("has.dot"));
}

TEST(IPFS, isIPFSurl) {
    // Valid IPFS URL
    EXPECT_TRUE(IPFS::isIPFSurl("ipfs://bafkreicr2pggml4j5bjv3hhxi5i5ud4rgnnaqphrfs2mtoub77zfiwbhju"));

    // Non-IPFS schemes
    EXPECT_FALSE(IPFS::isIPFSurl("http://example.com/file.png"));
    EXPECT_FALSE(IPFS::isIPFSurl("https://example.com/file.png"));
    EXPECT_FALSE(IPFS::isIPFSurl(""));

    // Prefix only (no CID after)
    EXPECT_FALSE(IPFS::isIPFSurl("ipfs://"));

    // CID with invalid character after prefix
    EXPECT_FALSE(IPFS::isIPFSurl("ipfs://has space"));
}

TEST(IPFS, getCID) {
    const string cid = "bafkreicr2pggml4j5bjv3hhxi5i5ud4rgnnaqphrfs2mtoub77zfiwbhju";
    EXPECT_EQ(IPFS::getCID("ipfs://" + cid), cid);

    // Throws on non-IPFS URL
    EXPECT_THROW(IPFS::getCID("http://example.com"), std::out_of_range);
    EXPECT_THROW(IPFS::getCID(""), std::out_of_range);
    EXPECT_THROW(IPFS::getCID(cid), std::out_of_range); // bare CID without scheme
}

TEST(IPFS, isLostCID) {
    // Known lost CID (first entry from _knownLostCID)
    EXPECT_TRUE(IPFS::isLostCID("bafkreiabavnsbsrrlfgisxcgmd7ontytbyh2ilruux7gjfc2hzi4qa5vxy"));

    // Valid but not in the lost list
    EXPECT_FALSE(IPFS::isLostCID("bafkreicr2pggml4j5bjv3hhxi5i5ud4rgnnaqphrfs2mtoub77zfiwbhju"));
    EXPECT_FALSE(IPFS::isLostCID(""));
    EXPECT_FALSE(IPFS::isLostCID("QmNotInTheLostList"));
}