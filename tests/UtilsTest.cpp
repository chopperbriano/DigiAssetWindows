//
// Tests for utils namespace — no external dependencies required.
//

#include "utils.h"
#include "gtest/gtest.h"

#include <array>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// split()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utils, split_byComma) {
    auto parts = utils::split("a,b,c", ',');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");
}

TEST(Utils, split_singleElement) {
    auto parts = utils::split("hello", ',');
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0], "hello");
}

TEST(Utils, split_emptyString) {
    auto parts = utils::split("", ',');
    EXPECT_TRUE(parts.empty());
}

TEST(Utils, split_bySlash) {
    auto parts = utils::split("a/b/c/d", '/');
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[3], "d");
}

TEST(Utils, split_consecutiveDelimiters) {
    // getline with delimiter produces empty tokens between consecutive delimiters
    auto parts = utils::split("a,,b", ',');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[1], "");
}

// ─────────────────────────────────────────────────────────────────────────────
// generateRandom()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utils, generateRandom_correctLength) {
    EXPECT_EQ(utils::generateRandom(10, utils::CodeType::NUMERIC).size(), 10u);
    EXPECT_EQ(utils::generateRandom(5,  utils::CodeType::UPPERCASE).size(), 5u);
    EXPECT_EQ(utils::generateRandom(20, utils::CodeType::ALPHANUMERIC).size(), 20u);
}

TEST(Utils, generateRandom_numericCharsOnly) {
    string s = utils::generateRandom(100, utils::CodeType::NUMERIC);
    for (char c : s) {
        EXPECT_TRUE(c >= '0' && c <= '9') << "Non-numeric char: " << c;
    }
}

TEST(Utils, generateRandom_uppercaseCharsOnly) {
    string s = utils::generateRandom(100, utils::CodeType::UPPERCASE);
    for (char c : s) {
        EXPECT_TRUE(c >= 'A' && c <= 'Z') << "Non-uppercase char: " << c;
    }
}

TEST(Utils, generateRandom_alphanumericCharsOnly) {
    string s = utils::generateRandom(200, utils::CodeType::ALPHANUMERIC);
    for (char c : s) {
        bool valid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
        EXPECT_TRUE(valid) << "Invalid alphanumeric char: " << c;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// fileExists()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utils, fileExists_existingFile) {
    // Create a temp file
    const string path = "/tmp/utils_test_exists.tmp";
    ofstream f(path);
    f << "test";
    f.close();

    EXPECT_TRUE(utils::fileExists(path));
    remove(path.c_str());
}

TEST(Utils, fileExists_nonExistentFile) {
    EXPECT_FALSE(utils::fileExists("/tmp/this_file_should_not_exist_utils_test.tmp"));
}

// ─────────────────────────────────────────────────────────────────────────────
// isInteger()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utils, isInteger_positiveInt) {
    EXPECT_TRUE(utils::isInteger("42"));
    EXPECT_TRUE(utils::isInteger("0"));
    EXPECT_TRUE(utils::isInteger("-1"));
}

TEST(Utils, isInteger_notAnInt) {
    EXPECT_FALSE(utils::isInteger("3.14"));
    EXPECT_FALSE(utils::isInteger("abc"));
    EXPECT_FALSE(utils::isInteger("12abc"));
    EXPECT_FALSE(utils::isInteger(""));
}

TEST(Utils, isInteger_leadingWhitespace_accepted) {
    // The implementation uses istringstream >> which skips leading whitespace,
    // so " 5" is treated as a valid integer.
    EXPECT_TRUE(utils::isInteger(" 5"));
}

// ─────────────────────────────────────────────────────────────────────────────
// copyFile()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utils, copyFile_success) {
    const string src = "/tmp/utils_copy_src.tmp";
    const string dst = "/tmp/utils_copy_dst.tmp";

    ofstream f(src);
    f << "hello copy";
    f.close();

    EXPECT_TRUE(utils::copyFile(src, dst));

    ifstream check(dst);
    string content((istreambuf_iterator<char>(check)), istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello copy");

    remove(src.c_str());
    remove(dst.c_str());
}

TEST(Utils, copyFile_sourceMissing) {
    EXPECT_FALSE(utils::copyFile("/tmp/nonexistent_utils_src.tmp", "/tmp/utils_copy_dst2.tmp"));
}

// ─────────────────────────────────────────────────────────────────────────────
// estimateJsonMemoryUsage()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utils, estimateJsonMemoryUsage_nullValue) {
    Json::Value v;
    size_t usage = utils::estimateJsonMemoryUsage(v);
    EXPECT_GE(usage, sizeof(Json::Value));
}

TEST(Utils, estimateJsonMemoryUsage_stringValue) {
    Json::Value v("hello");
    size_t usage = utils::estimateJsonMemoryUsage(v);
    // Should include sizeof(Json::Value) + at least 5 bytes for "hello"
    EXPECT_GE(usage, sizeof(Json::Value) + 5);
}

TEST(Utils, estimateJsonMemoryUsage_arrayValue) {
    Json::Value arr(Json::arrayValue);
    arr.append("abc");
    arr.append("defgh");
    size_t usage = utils::estimateJsonMemoryUsage(arr);
    // Should be larger than a single Json::Value
    EXPECT_GT(usage, sizeof(Json::Value));
}

TEST(Utils, estimateJsonMemoryUsage_objectValue) {
    Json::Value obj(Json::objectValue);
    obj["key"] = "value";
    size_t usage = utils::estimateJsonMemoryUsage(obj);
    EXPECT_GT(usage, sizeof(Json::Value));
}

// ─────────────────────────────────────────────────────────────────────────────
// mod256by64()
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utils, mod256by64_zeroNumerator) {
    array<uint8_t, 32> num = {};
    EXPECT_EQ(utils::mod256by64(num, 7), 0u);
}

TEST(Utils, mod256by64_oneValue) {
    // Set the lowest byte to 1 → value is 1
    array<uint8_t, 32> num = {};
    num[31] = 1;
    EXPECT_EQ(utils::mod256by64(num, 7), 1u % 7u);
}

TEST(Utils, mod256by64_knownValue) {
    // 256 mod 10 = 6
    // 256 = 0x100, so byte 30 = 0x01, byte 31 = 0x00
    array<uint8_t, 32> num = {};
    num[30] = 0x01; // 256
    EXPECT_EQ(utils::mod256by64(num, 10), 256u % 10u);
}

TEST(Utils, mod256by64_divisorOne) {
    array<uint8_t, 32> num = {};
    num[31] = 0xFF;
    EXPECT_EQ(utils::mod256by64(num, 1), 0u);
}

TEST(Utils, mod256by64_allFF) {
    array<uint8_t, 32> num;
    num.fill(0xFF);
    uint64_t divisor = 1000000007ULL;
    // Just verify it doesn't crash and returns < divisor
    uint64_t result = utils::mod256by64(num, divisor);
    EXPECT_LT(result, divisor);
}
