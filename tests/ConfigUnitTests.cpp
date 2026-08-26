#include "gtest/gtest.h"
#include "Config.h"
#include <fstream>
#include <cstdio>

class ConfigTest : public ::testing::Test {
protected:
    std::string tempFile = "_test_config_unit.cfg";

    void writeConfig(const std::string& content) {
        std::ofstream f(tempFile);
        f << content;
        f.close();
    }

    void TearDown() override {
        std::remove(tempFile.c_str());
    }
};

TEST_F(ConfigTest, ParseValidConfig) {
    writeConfig("rpcuser=fred\nrpcport=14022\nenabled=1\n");
    Config c(tempFile);
    EXPECT_EQ(c.getString("rpcuser"), "fred");
    EXPECT_EQ(c.getInteger("rpcport"), 14022);
    EXPECT_EQ(c.getBool("enabled"), true);
}

TEST_F(ConfigTest, MissingKeyThrows) {
    writeConfig("foo=bar\n");
    Config c(tempFile);
    EXPECT_THROW(c.getString("missing"), Config::exceptionCorruptConfigFile);
}

TEST_F(ConfigTest, DefaultValues) {
    writeConfig("");
    Config c(tempFile);
    EXPECT_EQ(c.getString("missing", "default"), "default");
    EXPECT_EQ(c.getInteger("missing", 42), 42);
    EXPECT_EQ(c.getBool("missing", true), true);
}

TEST_F(ConfigTest, BoolParsing) {
    writeConfig("a=true\nb=false\nc=1\nd=0\n");
    Config c(tempFile);
    EXPECT_TRUE(c.getBool("a"));
    EXPECT_FALSE(c.getBool("b"));
    EXPECT_TRUE(c.getBool("c"));
    EXPECT_FALSE(c.getBool("d"));
}

TEST_F(ConfigTest, CommentsAndBlankLines) {
    writeConfig("# comment\n\nfoo=bar\n# another comment\nbaz=42\n");
    Config c(tempFile);
    EXPECT_EQ(c.getString("foo"), "bar");
    EXPECT_EQ(c.getInteger("baz"), 42);
}

TEST_F(ConfigTest, WriteAndReRead) {
    Config c;
    c.setString("user", "alice");
    c.setInteger("port", 8080);
    c.setBool("enabled", true);
    c.write(tempFile);

    Config c2(tempFile);
    EXPECT_EQ(c2.getString("user"), "alice");
    EXPECT_EQ(c2.getInteger("port"), 8080);
    EXPECT_TRUE(c2.getBool("enabled"));
}

TEST_F(ConfigTest, MapPrefixFiltering) {
    writeConfig("rpcallowfoo=1\nrpcallowbar=0\nunrelated=5\n");
    Config c(tempFile);
    auto m = c.getBoolMap("rpcallow");
    EXPECT_EQ(m.size(), 2u);
    EXPECT_TRUE(m["foo"]);
    EXPECT_FALSE(m["bar"]);
}

// --- Indented comments (regression) -------------------------------------------
//
// A comment is any line whose first non-whitespace character is #. Only column 0 used to
// count, so "   # psp2costpercent=100" parsed as a KEY. That was harmless until the
// placeholder check landed: the key contains #, so the node refused to start, reporting
// that the setting "does nothing" while actually preventing startup entirely. Indenting a
// comment is an ordinary thing to do when hand-editing a config.
TEST(ConfigIndentedComments, IndentedCommentIsNotParsedAsAKey) {
    const std::string file = "_testIndentedComment.cfg";
    std::ofstream out(file);
    out << "rpcuser=alice\n"
        << "   # psp2costpercent=100\n"          //spaces
        << "\t# psp2mincostcents=0\n"            //tab
        << "#psp3payout=D123\n"                  //column 0, the case that always worked
        << "   \n"                               //whitespace only
        << "rpcport=14022\n";
    out.close();

    Config config(file);
    EXPECT_EQ(config.getString("rpcuser"), "alice");
    EXPECT_EQ(config.getInteger("rpcport"), 14022);
    // None of the commented lines may become keys - a key holding # is what stops startup.
    EXPECT_TRUE(config.getPlaceholderKeys().empty());
    std::remove(file.c_str());
}

// The check must still catch the mistake it exists for: example.cfg documents the pool
// options as psp#..., and copying one over literally leaves a key that silently does
// nothing (and for subscribe, leaves the node subscribed to a pool it meant to leave).
TEST(ConfigIndentedComments, RealPlaceholderKeyIsStillCaught) {
    const std::string file = "_testRealPlaceholder.cfg";
    std::ofstream out(file);
    out << "rpcuser=alice\n"
        << "psp#subscribe=1\n";
    out.close();

    Config config(file);
    const std::vector<std::string> keys = config.getPlaceholderKeys();
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0], "psp#subscribe");
    std::remove(file.c_str());
}
