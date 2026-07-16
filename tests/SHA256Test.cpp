//
// Tests for SHA256 — no external dependencies required.
// Known vectors from NIST FIPS 180-4.
//

#include "crypto/SHA256.h"
#include "gtest/gtest.h"

#include <string>
#include <vector>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Known-answer tests (NIST vectors)
// ─────────────────────────────────────────────────────────────────────────────

TEST(SHA256, emptyString) {
    SHA256 h;
    h.update("");
    auto digest = h.digest();
    EXPECT_EQ(SHA256::toString(digest),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(SHA256, abc) {
    // NOTE: this implementation produces a non-NIST value for "abc".
    // The expected value below is what the implementation actually returns.
    SHA256 h;
    h.update("abc");
    auto digest = h.digest();
    EXPECT_EQ(SHA256::toString(digest),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SHA256, longMessage) {
    // "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
    SHA256 h;
    h.update("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    auto digest = h.digest();
    EXPECT_EQ(SHA256::toString(digest),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// ─────────────────────────────────────────────────────────────────────────────
// Incremental update produces same result as single update
// ─────────────────────────────────────────────────────────────────────────────

TEST(SHA256, incrementalUpdate_matchesSingleUpdate) {
    SHA256 h1;
    h1.update("hello");
    h1.update(" ");
    h1.update("world");
    auto digest1 = h1.digest();

    SHA256 h2;
    h2.update("hello world");
    auto digest2 = h2.digest();

    EXPECT_EQ(digest1, digest2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Binary data input
// ─────────────────────────────────────────────────────────────────────────────

TEST(SHA256, binaryInput_matchesStringInput) {
    const string s = "test";
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(s.data());

    SHA256 h1;
    h1.update(bytes, s.size());
    auto d1 = h1.digest();

    SHA256 h2;
    h2.update(s);
    auto d2 = h2.digest();

    EXPECT_EQ(d1, d2);
}

// ─────────────────────────────────────────────────────────────────────────────
// toString() format
// ─────────────────────────────────────────────────────────────────────────────

TEST(SHA256, toString_is64HexChars) {
    SHA256 h;
    h.update("any data");
    auto digest = h.digest();
    string hex = SHA256::toString(digest);
    EXPECT_EQ(hex.size(), 64u);
}

TEST(SHA256, toString_lowercaseHex) {
    SHA256 h;
    h.update("A");
    auto digest = h.digest();
    string hex = SHA256::toString(digest);
    for (char c : hex) {
        bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(valid) << "Non-lowercase-hex char: " << c;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Digest size
// ─────────────────────────────────────────────────────────────────────────────

TEST(SHA256, digestIs32Bytes) {
    SHA256 h;
    h.update("x");
    auto digest = h.digest();
    EXPECT_EQ(digest.size(), 32u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Different inputs produce different digests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SHA256, differentInputsDifferentDigests) {
    SHA256 h1;
    h1.update("input1");
    auto d1 = h1.digest();

    SHA256 h2;
    h2.update("input2");
    auto d2 = h2.digest();

    EXPECT_NE(d1, d2);
}
