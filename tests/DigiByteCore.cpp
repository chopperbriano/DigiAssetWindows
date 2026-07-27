//
// Created by mctrivia on 27/05/23.
//

#include "DigiByteCore.h"
#include "Config.h"
#include "gtest/gtest.h"
#include <cmath>

using namespace std;

namespace {
    // True if a DigiByte Core is reachable with the local config.cfg. Tests that
    // need a live node SKIP (rather than fail) when it isn't - keeps CI green on a
    // bare runner with no wallet. Requires being run from a dir with config.cfg.
    bool coreReachable() {
        try {
            DigiByteCore c;
            c.setFileName("config.cfg");
            c.makeConnection();
            return c.getBlockCount() > 0;
        } catch (...) {
            return false;
        }
    }
} // namespace


TEST(DigiByteCore, MakeConnection) {
    if (!coreReachable()) GTEST_SKIP() << "no DigiByte Core reachable (config.cfg / wallet offline)";
    ///This will only pass if _DigiByteCore has the correct settings.  Following test will check this and output warning

    //try connecting with valid config
    DigiByteCore test;
    test.setFileName("config.cfg");
    bool failed = false;
    try {
        test.makeConnection();
    } catch (const Config::exceptionConfigFileInvalid& e) {
        failed = true;
        cout << "\n\n";
        cout << "***********************************************************\n";
        cout << "* Test failed because bin/config.cfg is not set correctly *\n";
        cout << "* for this wallet. Please fix and re run tests.           *\n";
        cout << "***********************************************************\n\n";
    } catch (const Config::exceptionCorruptConfigFile& e) {
        failed = true;
        cout << "\n\n";
        cout << "*******************************************************************************\n";
        cout << "* Test failed because bin/config.cfg is corrupt. Please fix and re run tests. *\n";
        cout << "*******************************************************************************\n\n";
    } catch (const Config::exceptionConfigFileMissing& e) {
        failed = true;
        cout << "\n\n";
        cout << "*******************************************************************************\n";
        cout << "* Test failed because bin/config.cfg is missing. Please fix and re run tests. *\n";
        cout << "*******************************************************************************\n\n";
    } catch (const DigiByteCore::exceptionCoreOffline& e) {
        failed = true;
        cout << "\n\n";
        cout << "****************************************************************************\n";
        cout << "* Test failed because Core Wallet is offline. Please fix and re run tests. *\n";
        cout << "****************************************************************************\n\n";
    } catch (const std::exception& e) {
        failed = true;
    }
    EXPECT_GT(test.getBlockCount(), 1);
    EXPECT_FALSE(failed);

    //change to missing config file
    test.setFileName("../tests/testFiles/DigiByteCore_bad0.cfg");
    failed = true;
    try {
        test.makeConnection();
    } catch (const Config::exceptionConfigFileMissing& e) {
        failed = false; //want this exception
    } catch (const std::exception& e) {
        failed = true;
    }
    EXPECT_FALSE(failed);

    //change to bad config file
    test.setFileName("../tests/testFiles/DigiByteCore_bad1.cfg");
    failed = true;
    try {
        test.makeConnection();
    } catch (const Config::exceptionConfigFileInvalid& e) {
        failed = false; //want this exception
    } catch (const std::exception& e) {
        failed = true;
    }
    EXPECT_FALSE(failed);

    //change to corrupt config file
    test.setFileName("../tests/testFiles/DigiByteCore_bad2.cfg");
    failed = true;
    try {
        test.makeConnection();
    } catch (const Config::exceptionCorruptConfigFile& e) {
        failed = false; //want this exception
    } catch (const std::exception& e) {
        failed = true;
    }
    EXPECT_FALSE(failed);
}

TEST(DigiByteCore, GetBlockCount) {
    if (!coreReachable()) GTEST_SKIP() << "no DigiByte Core reachable";
    DigiByteCore test;
    test.setFileName("config.cfg");
    test.makeConnection();
    EXPECT_GT(test.getBlockCount(), 1);
}

TEST(DigiByteCore, GetBlockHash) {
    if (!coreReachable()) GTEST_SKIP() << "no DigiByte Core reachable";
    //test a block that should be synced
    DigiByteCore test;
    test.setFileName("config.cfg");
    test.makeConnection();
    EXPECT_EQ(test.getBlockHash(1), "4da631f2ac1bed857bd968c67c913978274d8aabed64ab2bcebc1665d7f4d3a0");

    //test a block that probably does not exist yet
    bool failed = true;
    try {
        test.getBlockHash(1000000000);
    } catch (const DigiByteCore::exception& e) {
        failed = false; //want this exception
    } catch (const std::exception& e) {
        failed = true;
    }
    EXPECT_FALSE(failed);
}