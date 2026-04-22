//
// Tests for ChainAnalyzer — uses loadFake() to avoid needing a live node.
//

#include "AppMain.h"
#include "ChainAnalyzer.h"
#include "Database.h"
#include "gtest/gtest.h"

#include <cstdio>
#include <string>

using namespace std;

static const string DB_PATH = "../tests/testFiles/_chainanalyzer_test.db";

class ChainAnalyzerTest : public ::testing::Test {
protected:
    AppMain* appMain = nullptr;
    Database* db = nullptr;
    ChainAnalyzer* analyzer = nullptr;

    void SetUp() override {
        remove(DB_PATH.c_str());
        remove((DB_PATH + "-wal").c_str());
        remove((DB_PATH + "-shm").c_str());

        appMain = AppMain::GetInstance();
        db = new Database(DB_PATH);
        appMain->setDatabase(db);

        analyzer = new ChainAnalyzer();
    }

    void TearDown() override {
        appMain->reset();
        delete analyzer;
        delete db;
        remove(DB_PATH.c_str());
        remove((DB_PATH + "-wal").c_str());
        remove((DB_PATH + "-shm").c_str());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Initial state
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ChainAnalyzerTest, initialState_isStopped) {
    EXPECT_EQ(analyzer->getSync(), +ChainAnalyzer::STOPPED);
}

// ─────────────────────────────────────────────────────────────────────────────
// loadFake() — sets state without needing a live node
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ChainAnalyzerTest, loadFake_setsSyncedState) {
    analyzer->loadFake(1000, +ChainAnalyzer::SYNCED);
    EXPECT_EQ(analyzer->getSync(), +ChainAnalyzer::SYNCED);
}

TEST_F(ChainAnalyzerTest, loadFake_setsSyncHeight) {
    analyzer->loadFake(17579454, +ChainAnalyzer::SYNCED);
    EXPECT_EQ(analyzer->getSyncHeight(), 17579454u);
}

TEST_F(ChainAnalyzerTest, loadFake_setsNegativeSyncLevel) {
    // Negative sync = how many blocks behind
    analyzer->loadFake(1000, -50);
    EXPECT_EQ(analyzer->getSync(), -50);
}

TEST_F(ChainAnalyzerTest, loadFake_stoppedState) {
    analyzer->loadFake(500, +ChainAnalyzer::STOPPED);
    EXPECT_EQ(analyzer->getSync(), +ChainAnalyzer::STOPPED);
}

TEST_F(ChainAnalyzerTest, loadFake_initializingState) {
    analyzer->loadFake(0, +ChainAnalyzer::INITIALIZING);
    EXPECT_EQ(analyzer->getSync(), +ChainAnalyzer::INITIALIZING);
}

// ─────────────────────────────────────────────────────────────────────────────
// Config setters (no assertions on stored values — just verify no crash)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(ChainAnalyzerTest, setPruneAge_doesNotThrow) {
    EXPECT_NO_THROW(analyzer->setPruneAge(1440));
    EXPECT_NO_THROW(analyzer->setPruneAge(-1));
}

TEST_F(ChainAnalyzerTest, setPruneFlags_doesNotThrow) {
    EXPECT_NO_THROW(analyzer->setPruneExchangeHistory(true));
    EXPECT_NO_THROW(analyzer->setPruneUTXOHistory(false));
    EXPECT_NO_THROW(analyzer->setPruneVoteHistory(true));
    EXPECT_NO_THROW(analyzer->setStoreNonAssetUTXO(false));
}

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

TEST(ChainAnalyzer_Constants, stateConstants_areDistinct) {
    EXPECT_NE(+ChainAnalyzer::SYNCED,       +ChainAnalyzer::STOPPED);
    EXPECT_NE(+ChainAnalyzer::STOPPED,      +ChainAnalyzer::INITIALIZING);
    EXPECT_NE(+ChainAnalyzer::INITIALIZING, +ChainAnalyzer::REWINDING);
    EXPECT_NE(+ChainAnalyzer::REWINDING,    +ChainAnalyzer::BUSY);
}

TEST(ChainAnalyzer_Constants, synced_isZero) {
    EXPECT_EQ(+ChainAnalyzer::SYNCED, 0);
}
