// Regression tests for Database::buildTables() version migration.
//
// The 2026-08 upstream merge produced a bug that would have broken every existing node on
// upgrade: both this fork and upstream had implemented DigiDollar indexing independently, so
// the merge kept TWO "version 6 to version 7" migration lambdas. A database already at
// version 7 ran the second one and tried to CREATE TABLE ddutxos over itself, throwing
// "Table creation failed" on open. Nothing covered the migration path, so only a full test
// run against a database that had been closed and reopened caught it.
//
// These tests pin the properties that were violated:
//   - opening a fresh database twice does not re-run creation over itself
//   - a database at an older version migrates forward and lands on the current version
//   - a migration is safe to re-run (every statement is IF NOT EXISTS / OR IGNORE), so a
//     node interrupted mid-migration heals on the next start instead of erroring forever

#include "Database.h"
#include "gtest/gtest.h"
#include <cstdio>
#include <sqlite3.h>
#include <string>

namespace {

    const unsigned int CURRENT_DB_VERSION = 7;

    class DatabaseMigrationTest : public ::testing::Test {
    protected:
        std::string dbFile;

        void SetUp() override {
            static int counter = 0;
            dbFile = "_testMigrate_" + std::to_string(counter++) + ".db";
            removeDbFiles();
        }

        void TearDown() override { removeDbFiles(); }

        void removeDbFiles() {
            std::remove(dbFile.c_str());
            std::remove((dbFile + "-wal").c_str());
            std::remove((dbFile + "-shm").c_str());
        }

        // Reads flags.dbVersion straight out of the file, without going through Database -
        // the point is to observe what was actually persisted.
        int readDbVersion() {
            sqlite3* raw = nullptr;
            if (sqlite3_open(dbFile.c_str(), &raw) != SQLITE_OK) {
                sqlite3_close(raw);
                return -1;
            }
            int version = -1;
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(raw, "SELECT value FROM flags WHERE key='dbVersion';", -1, &stmt, nullptr) ==
                SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(raw);
            return version;
        }

        // Forces the stored version backwards so the next open has to migrate forward.
        bool setDbVersion(int version) {
            sqlite3* raw = nullptr;
            if (sqlite3_open(dbFile.c_str(), &raw) != SQLITE_OK) {
                sqlite3_close(raw);
                return false;
            }
            const std::string sql = "UPDATE flags SET value=" + std::to_string(version) + " WHERE key='dbVersion';";
            const bool ok = (sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
            return ok;
        }

        // Drops the DigiDollar tables so a forced-back version has something real to migrate.
        bool dropDigiDollarTables() {
            sqlite3* raw = nullptr;
            if (sqlite3_open(dbFile.c_str(), &raw) != SQLITE_OK) {
                sqlite3_close(raw);
                return false;
            }
            const char* sql = "DROP TABLE IF EXISTS ddutxos;"
                              "DROP TABLE IF EXISTS ddvaults;"
                              "DROP TABLE IF EXISTS ddoracle;";
            const bool ok = (sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
            return ok;
        }

        bool tableExists(const std::string& name) {
            sqlite3* raw = nullptr;
            if (sqlite3_open(dbFile.c_str(), &raw) != SQLITE_OK) {
                sqlite3_close(raw);
                return false;
            }
            const std::string sql =
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='" + name + "';";
            sqlite3_stmt* stmt = nullptr;
            int count = 0;
            if (sqlite3_prepare_v2(raw, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(raw);
            return count > 0;
        }
    };

    // A fresh database is created at the current version, with the DigiDollar tables present.
    TEST_F(DatabaseMigrationTest, FreshDatabaseIsCurrentVersion) {
        { Database db(dbFile); }
        EXPECT_EQ(readDbVersion(), static_cast<int>(CURRENT_DB_VERSION));
        EXPECT_TRUE(tableExists("ddutxos"));
        EXPECT_TRUE(tableExists("ddvaults"));
        EXPECT_TRUE(tableExists("ddoracle"));
    }

    // THE REGRESSION. Reopening a current database must not replay creation or any migration
    // over it. The duplicated 6->7 lambda made this throw "table ddutxos already exists".
    TEST_F(DatabaseMigrationTest, ReopeningCurrentDatabaseDoesNotRemigrate) {
        { Database db(dbFile); }
        EXPECT_NO_THROW({ Database db(dbFile); });
        EXPECT_EQ(readDbVersion(), static_cast<int>(CURRENT_DB_VERSION));
    }

    // Opening repeatedly must stay stable - a migration that fired once per open would show up
    // here even if a single reopen happened to survive.
    TEST_F(DatabaseMigrationTest, RepeatedOpensAreStable) {
        { Database db(dbFile); }
        for (int i = 0; i < 5; i++) {
            EXPECT_NO_THROW({ Database db(dbFile); }) << "failed on open " << (i + 2);
            EXPECT_EQ(readDbVersion(), static_cast<int>(CURRENT_DB_VERSION));
        }
    }

    // A version 6 database migrates forward, creating the DigiDollar tables and landing on 7.
    TEST_F(DatabaseMigrationTest, MigratesFromVersion6) {
        { Database db(dbFile); }
        ASSERT_TRUE(dropDigiDollarTables());
        ASSERT_TRUE(setDbVersion(6));
        ASSERT_FALSE(tableExists("ddutxos"));

        EXPECT_NO_THROW({ Database db(dbFile); });
        EXPECT_EQ(readDbVersion(), static_cast<int>(CURRENT_DB_VERSION));
        EXPECT_TRUE(tableExists("ddutxos"));
        EXPECT_TRUE(tableExists("ddvaults"));
        EXPECT_TRUE(tableExists("ddoracle"));
    }

    // A node killed part way through the 6->7 migration leaves the tables in place but the
    // version still at 6. The next start re-runs the migration, so every statement in it has
    // to tolerate what is already there rather than erroring out forever.
    TEST_F(DatabaseMigrationTest, InterruptedMigrationIsRerunnable) {
        { Database db(dbFile); }
        ASSERT_TRUE(setDbVersion(6)); //tables kept: exactly the half-migrated state
        ASSERT_TRUE(tableExists("ddutxos"));

        EXPECT_NO_THROW({ Database db(dbFile); }) << "re-running the 6->7 migration must not throw";
        EXPECT_EQ(readDbVersion(), static_cast<int>(CURRENT_DB_VERSION));
        EXPECT_TRUE(tableExists("ddutxos"));
    }

}// namespace
