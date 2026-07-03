#include "gtest/gtest.h"
#include "Version.h"
#include <string>
#include <regex>

TEST(Version, VersionStringFormat) {
    std::string v = getVersionString();
    // Should match pattern like "0.3.0-win.7"
    std::regex pattern(R"(\d+\.\d+\.\d+-win\.\d+)");
    EXPECT_TRUE(std::regex_match(v, pattern)) << "Version string: " << v;
}

TEST(Version, ProductVersionString) {
    std::string pv = getProductVersionString();
    EXPECT_TRUE(pv.find("DigiAsset for Windows v") == 0) << "Product version: " << pv;
}

TEST(Version, UpstreamVersionString) {
    std::string uv = getUpstreamVersionString();
    std::regex pattern(R"(\d+\.\d+\.\d+)");
    EXPECT_TRUE(std::regex_match(uv, pattern)) << "Upstream version: " << uv;
}

TEST(Version, ProductContainsVersion) {
    std::string pv = getProductVersionString();
    std::string v = getVersionString();
    EXPECT_TRUE(pv.find(v) != std::string::npos) << "Product: " << pv << " should contain: " << v;
}
