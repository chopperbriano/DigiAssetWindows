//
// Tests for Blob class — no external dependencies required.
//

#include "Blob.h"
#include "gtest/gtest.h"

#include <stdexcept>
#include <vector>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor from void* + length
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, voidPtrConstructor_storesData) {
    uint8_t raw[] = {0x01, 0x02, 0x03, 0xFF};
    Blob b(raw, 4);
    EXPECT_EQ(b.length(), 4u);
    EXPECT_EQ(b.data()[0], 0x01);
    EXPECT_EQ(b.data()[3], 0xFF);
}

TEST(Blob, voidPtrConstructor_zeroLength) {
    Blob b(nullptr, 0);
    EXPECT_EQ(b.length(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor from vector<uint8_t>
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, vectorConstructor_storesData) {
    vector<uint8_t> v = {0xAA, 0xBB, 0xCC};
    Blob b(v);
    EXPECT_EQ(b.length(), 3u);
    EXPECT_EQ(b.data()[0], 0xAA);
    EXPECT_EQ(b.data()[2], 0xCC);
}

TEST(Blob, vectorConstructor_emptyVector) {
    vector<uint8_t> v;
    Blob b(v);
    EXPECT_EQ(b.length(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor from hex string
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, hexConstructor_lowercase) {
    Blob b("deadbeef");
    EXPECT_EQ(b.length(), 4u);
    EXPECT_EQ(b.data()[0], 0xDE);
    EXPECT_EQ(b.data()[1], 0xAD);
    EXPECT_EQ(b.data()[2], 0xBE);
    EXPECT_EQ(b.data()[3], 0xEF);
}

TEST(Blob, hexConstructor_uppercase) {
    Blob b("DEADBEEF");
    EXPECT_EQ(b.length(), 4u);
    EXPECT_EQ(b.data()[0], 0xDE);
    EXPECT_EQ(b.data()[3], 0xEF);
}

TEST(Blob, hexConstructor_emptyString) {
    Blob b("");
    EXPECT_EQ(b.length(), 0u);
}

TEST(Blob, hexConstructor_oddLengthThrows) {
    EXPECT_THROW(Blob("abc"), std::invalid_argument);
}

TEST(Blob, hexConstructor_invalidCharThrows) {
    EXPECT_THROW(Blob("zz"), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// toHex()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, toHex_producesLowercaseHex) {
    vector<uint8_t> v = {0xDE, 0xAD, 0xBE, 0xEF};
    Blob b(v);
    EXPECT_EQ(b.toHex(), "deadbeef");
}

TEST(Blob, toHex_zeroBytes) {
    vector<uint8_t> v = {0x00, 0x00};
    Blob b(v);
    EXPECT_EQ(b.toHex(), "0000");
}

TEST(Blob, toHex_roundTrip) {
    string hex = "0102030405060708090a0b0c0d0e0f10";
    Blob b(hex);
    EXPECT_EQ(b.toHex(), hex);
}

// ─────────────────────────────────────────────────────────────────────────────
// vector()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, vectorMethod_returnsCorrectData) {
    vector<uint8_t> input = {0x11, 0x22, 0x33};
    Blob b(input);
    vector<uint8_t> output = b.vector();
    EXPECT_EQ(input, output);
}

TEST(Blob, vectorMethod_independentCopy) {
    vector<uint8_t> input = {0x01, 0x02};
    Blob b(input);
    vector<uint8_t> output = b.vector();
    output[0] = 0xFF; // mutate the copy
    EXPECT_EQ(b.data()[0], 0x01); // original unchanged
}

// ─────────────────────────────────────────────────────────────────────────────
// Copy constructor
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, copyConstructor_deepCopy) {
    vector<uint8_t> v = {0x01, 0x02, 0x03};
    Blob a(v);
    Blob b(a);

    EXPECT_EQ(a.length(), b.length());
    EXPECT_EQ(a.toHex(), b.toHex());
    EXPECT_NE(a.data(), b.data()); // distinct buffers
}

TEST(Blob, copyConstructor_modifyOriginalDoesNotAffectCopy) {
    uint8_t raw[] = {0xAA, 0xBB};
    Blob a(raw, 2);
    Blob b(a);
    // Modify data through a (via toHex to verify they differ after raw change)
    EXPECT_EQ(b.toHex(), "aabb");
}

// ─────────────────────────────────────────────────────────────────────────────
// Assignment operator
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, assignmentOperator_deepCopy) {
    vector<uint8_t> v1 = {0x01, 0x02};
    vector<uint8_t> v2 = {0x03, 0x04, 0x05};
    Blob a(v1);
    Blob b(v2);
    b = a;

    EXPECT_EQ(b.length(), 2u);
    EXPECT_EQ(b.toHex(), a.toHex());
    EXPECT_NE(a.data(), b.data());
}

TEST(Blob, assignmentOperator_selfAssignment) {
    vector<uint8_t> v = {0xDE, 0xAD};
    Blob a(v);
    a = a; // should not crash or corrupt
    EXPECT_EQ(a.toHex(), "dead");
}

// ─────────────────────────────────────────────────────────────────────────────
// Equality operators
// ─────────────────────────────────────────────────────────────────────────────

TEST(Blob, equalityOperator_sameContent) {
    Blob a("aabbcc");
    Blob b("aabbcc");
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(Blob, equalityOperator_differentContent) {
    Blob a("aabbcc");
    Blob b("aabbdd");
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(Blob, equalityOperator_differentLength) {
    Blob a("aabb");
    Blob b("aabbcc");
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(Blob, equalityOperator_selfComparison) {
    Blob a("deadbeef");
    EXPECT_TRUE(a == a);
    EXPECT_FALSE(a != a);
}

TEST(Blob, equalityOperator_emptyBlobs) {
    Blob a("");
    Blob b("");
    EXPECT_TRUE(a == b);
}
