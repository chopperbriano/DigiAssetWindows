// MigrationTest.cpp - regression coverage for chain.db schema upgrades.
//
// Why this exists: every other database test starts by remove()ing its file and
// letting Database create a fresh one, so they all exercise the CURRENT schema
// built from scratch. Nothing covered the UPGRADE path - which is the path every
// already-deployed node actually takes. The v6 -> v7 DigiDollar migration shipped
// in win.124 with no test touching it at all.
//
// _testMigrationV6.db is a real v6 database, produced by the win.123 build (the
// last release before the DigiDollar tables existed). It is copied before use so
// the fixture itself stays pristine and the test can be re-run.
//
// When adding a future schema version: mint a fixture from the last release that
// predates it and add a case here, so the upgrade path never ships unverified again.

#include "Database.h"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <cstdio>
#include <string>

namespace {
    int scalarInt(sqlite3* db, const std::string& sql, int notFound = -999) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return notFound;
        int v = notFound;
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return v;
    }
    bool tableExists(sqlite3* db, const std::string& name) {
        return scalarInt(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='" + name + "';", 0) == 1;
    }
    int indexCount(sqlite3* db, const std::string& tbl) {
        return scalarInt(db, "SELECT count(*) FROM sqlite_master WHERE type='index' AND tbl_name='" + tbl + "';", 0);
    }

    const char* V6_FIXTURE = "../tests/testFiles/_testMigrationV6.db";
    const char* WORK = "../tests/testFiles/_testMigrationWork.db";

    //work on a copy so the committed fixture is never modified by a test run
    void freshCopyOfFixture() {
        remove(WORK);
        remove((std::string(WORK) + "-wal").c_str());
        remove((std::string(WORK) + "-shm").c_str());
        FILE* in = fopen(V6_FIXTURE, "rb");
        ASSERT_NE(in, nullptr) << "missing v6 fixture " << V6_FIXTURE;
        FILE* out = fopen(WORK, "wb");
        ASSERT_NE(out, nullptr);
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(in);
        fclose(out);
    }
}

/**
 * A v6 database must upgrade to v7 in place, keeping everything it already held.
 * If this breaks, deployed nodes cannot open their chain.db after updating.
 */
TEST(Migration, v6DatabaseUpgradesToV7) {
    freshCopyOfFixture();

    //the fixture must genuinely be v6, or this test proves nothing
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(WORK, &raw), SQLITE_OK);
        EXPECT_EQ(scalarInt(raw, "SELECT value FROM flags WHERE key='dbVersion';"), 6)
                << "fixture is not a v6 database";
        EXPECT_FALSE(tableExists(raw, "ddutxos"));
        EXPECT_FALSE(tableExists(raw, "ddvaults"));
        EXPECT_FALSE(tableExists(raw, "ddoracle"));
        sqlite3_close(raw);
    }

    //opening with the current build runs the migration in the constructor
    ASSERT_NO_THROW({ Database db(WORK); })
            << "migration threw - deployed nodes would fail to open chain.db";

    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(WORK, &raw), SQLITE_OK);

        EXPECT_EQ(scalarInt(raw, "SELECT value FROM flags WHERE key='dbVersion';"), 7);

        EXPECT_TRUE(tableExists(raw, "ddutxos"));
        EXPECT_TRUE(tableExists(raw, "ddvaults"));
        EXPECT_TRUE(tableExists(raw, "ddoracle"));

        //without the indexes the activation backfill and every holdings lookup table-scan
        EXPECT_GE(indexCount(raw, "ddutxos"), 3);
        EXPECT_GE(indexCount(raw, "ddvaults"), 3);
        EXPECT_GE(indexCount(raw, "ddoracle"), 1);

        //ddSyncHeight must exist and be 0, or ChainAnalyzer never runs the DigiDollar backfill
        //and the node silently reports no DigiDollar data forever
        EXPECT_EQ(scalarInt(raw, "SELECT count(*) FROM flags WHERE key='ddSyncHeight';"), 1);
        EXPECT_EQ(scalarInt(raw, "SELECT value FROM flags WHERE key='ddSyncHeight';"), 0);

        //pre-existing v6 state must survive untouched - a migration that drops chain data
        //would force every node into a full resync
        EXPECT_TRUE(tableExists(raw, "assets"));
        EXPECT_TRUE(tableExists(raw, "blocks"));
        EXPECT_TRUE(tableExists(raw, "utxos"));
        EXPECT_TRUE(tableExists(raw, "exchange"));
        EXPECT_TRUE(tableExists(raw, "domainsMasters"));

        sqlite3_close(raw);
    }
}

/**
 * Re-opening an already-migrated database must be a no-op. If a migration re-fired,
 * its CREATE TABLE would fail and the node would refuse to start on EVERY restart
 * after upgrading - which looks like a working upgrade followed by a dead node.
 */
TEST(Migration, reopeningMigratedDatabaseIsIdempotent) {
    freshCopyOfFixture();
    ASSERT_NO_THROW({ Database db(WORK); });    //first open migrates
    ASSERT_NO_THROW({ Database db(WORK); })     //second open must not re-run it
            << "re-opening a migrated database threw - nodes would fail on restart";

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(WORK, &raw), SQLITE_OK);
    EXPECT_EQ(scalarInt(raw, "SELECT value FROM flags WHERE key='dbVersion';"), 7);
    EXPECT_EQ(scalarInt(raw, "SELECT count(*) FROM flags WHERE key='ddSyncHeight';"), 1);
    sqlite3_close(raw);
}
