//
// Regression tests for the DigiByte v7/v8/v9 chain split of June 2026.
// Uses real fixtures captured from a synced v9.26.4 node (see
// tests/fixtures/chain-split-2026/README.md). No live Core needed - the parsers
// under test are pure/static.
//
//   #4: scriptPubKey address extraction across Core versions (v7 plural
//       "addresses"[] vs v8/8.22+ singular "address").
//   #3: parsing the anomalous Groestl block 23,751,096 (legacy version
//       0x00000400) - the block class that stalls older indexers.
//

#include "gtest/gtest.h"
#include "DigiByteCore.h"
#include <fstream>
#include <string>
#include <vector>

// Load a JSON fixture named relative to the chain-split fixture directory
// (e.g. "scriptpubkey/v8-v9-address-singular.json"). Resolution order:
//   1. CHAIN_SPLIT_FIXTURE_DIR - an absolute path baked in by CMake, so the test
//      passes no matter what CWD it is launched from (ctest / VS / a raw run out
//      of build/tests/Release each have a different CWD).
//   2. A few CWD-relative locations, as a fallback for ad-hoc/other build setups.
static bool loadJson(const std::string& rel, Json::Value& out) {
    std::vector<std::string> candidates;
#ifdef CHAIN_SPLIT_FIXTURE_DIR
    candidates.push_back(std::string(CHAIN_SPLIT_FIXTURE_DIR) + "/" + rel);
#endif
    const char* prefixes[] = {"", "tests/", "../tests/", "../../tests/", "../../../tests/"};
    for (const char* p : prefixes) candidates.push_back(std::string(p) + "fixtures/chain-split-2026/" + rel);

    for (const std::string& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) continue;
        Json::CharReaderBuilder b;
        std::string errs;
        if (Json::parseFromStream(b, f, &out, &errs)) return true;
    }
    return false;
}

static const char* kSpkV8 = "scriptpubkey/v8-v9-address-singular.json";
static const char* kSpkV7 = "scriptpubkey/v7-addresses-plural.json";
static const char* kGroestl = "groestl-blocks/block-23751096.json";

// #4 - v8/8.22+ singular "address" is extracted.
TEST(ChainSplit2026, ScriptPubKey_v8v9_SingularAddress) {
    Json::Value spk;
    ASSERT_TRUE(loadJson(kSpkV8, spk)) << "fixture not found: " << kSpkV8;
    std::vector<std::string> out;
    DigiByteCore::extractScriptPubKeyAddresses(spk, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], spk["address"].asString());
}

// #4 - v7.17.x plural "addresses"[] is extracted (same result, different shape).
TEST(ChainSplit2026, ScriptPubKey_v7_PluralAddresses) {
    Json::Value spk;
    ASSERT_TRUE(loadJson(kSpkV7, spk)) << "fixture not found: " << kSpkV7;
    std::vector<std::string> out;
    DigiByteCore::extractScriptPubKeyAddresses(spk, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], spk["addresses"][0u].asString());
}

// #3 - the indexer parses the Groestl block (legacy version 0x00000400) cleanly
// instead of choking on the unfamiliar version, which is the older-indexer bug.
TEST(ChainSplit2026, GroestlBlock_ParsesWithLegacyVersion) {
    Json::Value blk;
    ASSERT_TRUE(loadJson(kGroestl, blk)) << "fixture not found: " << kGroestl;
    blockinfo_t info = DigiByteCore::parseVerboseBlockForTest(blk);
    EXPECT_EQ(info.height, 23751096);
    EXPECT_EQ(info.version, 0x00000400);   // 1024 - retired-algo legacy version
    EXPECT_EQ(info.hash, "b2749cf0462eb6c35bf5f8f1d75f4891e62d3ca1d9861fac48b46c05ba078283");
    EXPECT_GE(info.tx.size(), 1u);         // coinbase parsed, no throw
}
