//
// Tests for Statement and LockedStatement — uses SQLite :memory: database.
//

#include "Database_Statement.h"
#include "Database_LockedStatement.h"
#include "gtest/gtest.h"

#include <chrono>
#include <sqlite3.h>
#include <string>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Fixture: in-memory SQLite database
// ─────────────────────────────────────────────────────────────────────────────

class StatementTest : public ::testing::Test {
protected:
    sqlite3* db = nullptr;

    void SetUp() override {
        int rc = sqlite3_open(":memory:", &db);
        ASSERT_EQ(rc, SQLITE_OK) << "Failed to open :memory: database";
        sqlite3_exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "INSERT INTO test VALUES (1, 'hello')", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "INSERT INTO test VALUES (2, 'world')", nullptr, nullptr, nullptr);
    }

    void TearDown() override {
        if (db) sqlite3_close(db);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Statement
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(StatementTest, defaultConstructor_zeroCountsAndDuration) {
    Statement stmt;
    EXPECT_EQ(stmt.getLockCount(), 0);
    EXPECT_EQ(stmt.getTotalLockDuration(), 0LL);
}

TEST_F(StatementTest, prepare_doesNotThrow) {
    Statement stmt;
    EXPECT_NO_THROW(stmt.prepare(db, "SELECT id, val FROM test WHERE id = ?"));
}

TEST_F(StatementTest, addLockDuration_accumulates) {
    Statement stmt;
    stmt.addLockDuration(chrono::microseconds(100));
    stmt.addLockDuration(chrono::microseconds(200));
    EXPECT_EQ(stmt.getTotalLockDuration(), 300LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// LockedStatement — bind, step, get column
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(StatementTest, lockedStatement_bindAndGetInt) {
    Statement stmt;
    stmt.prepare(db, "SELECT id FROM test WHERE id = ?");

    {
        LockedStatement ls(stmt);
        ls.bindInt(1, 1);
        int rc = ls.executeStep();
        EXPECT_EQ(rc, SQLITE_ROW);
        EXPECT_EQ(ls.getColumnInt(0), 1);
    }
}

TEST_F(StatementTest, lockedStatement_bindAndGetText) {
    Statement stmt;
    stmt.prepare(db, "SELECT val FROM test WHERE id = ?");

    {
        LockedStatement ls(stmt);
        ls.bindInt(1, 2);
        int rc = ls.executeStep();
        EXPECT_EQ(rc, SQLITE_ROW);
        EXPECT_EQ(ls.getColumnText(0), "world");
    }
}

TEST_F(StatementTest, lockedStatement_noMoreRows) {
    Statement stmt;
    stmt.prepare(db, "SELECT id FROM test WHERE id = 999");

    {
        LockedStatement ls(stmt);
        int rc = ls.executeStep();
        EXPECT_EQ(rc, SQLITE_DONE);
    }
}

TEST_F(StatementTest, lockedStatement_reset_allowsReuse) {
    Statement stmt;
    stmt.prepare(db, "SELECT id FROM test WHERE id = ?");

    {
        LockedStatement ls(stmt);
        ls.bindInt(1, 1);
        ls.executeStep();
        ls.reset();
        ls.bindInt(1, 2);
        int rc = ls.executeStep();
        EXPECT_EQ(rc, SQLITE_ROW);
        EXPECT_EQ(ls.getColumnInt(0), 2);
    }
}

TEST_F(StatementTest, lockedStatement_tracksDuration) {
    Statement stmt;
    stmt.prepare(db, "SELECT 1");

    {
        LockedStatement ls(stmt);
        ls.executeStep();
        // destructor records duration
    }
    EXPECT_EQ(stmt.getLockCount(), 1);
    EXPECT_GE(stmt.getTotalLockDuration(), 0LL);
}

TEST_F(StatementTest, lockedStatement_multipleUses_incrementsCount) {
    Statement stmt;
    stmt.prepare(db, "SELECT 1");

    for (int i = 0; i < 3; ++i) {
        LockedStatement ls(stmt);
        ls.executeStep();
    }
    EXPECT_EQ(stmt.getLockCount(), 3);
}

TEST_F(StatementTest, lockedStatement_bindInt64) {
    Statement stmt;
    stmt.prepare(db, "SELECT id FROM test WHERE id = ?");

    {
        LockedStatement ls(stmt);
        ls.bindInt64(1, 1LL);
        int rc = ls.executeStep();
        EXPECT_EQ(rc, SQLITE_ROW);
        EXPECT_EQ(ls.getColumnInt64(0), 1LL);
    }
}

TEST_F(StatementTest, lockedStatement_bindDouble) {
    sqlite3_exec(db, "CREATE TABLE doubles (v REAL)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO doubles VALUES (3.14)", nullptr, nullptr, nullptr);

    Statement stmt;
    stmt.prepare(db, "SELECT v FROM doubles");

    {
        LockedStatement ls(stmt);
        ls.executeStep();
        EXPECT_NEAR(ls.getColumnDouble(0), 3.14, 1e-9);
    }
}

TEST_F(StatementTest, lockedStatement_bindNull) {
    sqlite3_exec(db, "CREATE TABLE nullable (v TEXT)", nullptr, nullptr, nullptr);

    Statement stmt;
    stmt.prepare(db, "INSERT INTO nullable VALUES (?)");

    {
        LockedStatement ls(stmt);
        ls.bindNull(1);
        ls.executeStep();
    }

    Statement check;
    check.prepare(db, "SELECT v FROM nullable");
    {
        LockedStatement ls(check);
        ls.executeStep();
        // Column type should be NULL
        EXPECT_EQ(ls.getColumnText(0), "");
    }
}
