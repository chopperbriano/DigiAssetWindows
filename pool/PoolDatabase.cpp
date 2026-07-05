//
// PoolDatabase.cpp - implementation of the pool server's sqlite state store.
//
// One class method per query, each guarded by the instance mutex and using
// prepared statements. Beyond the straight CRUD accessors (documented on their
// declarations in PoolDatabase.h) this file also contains the schema
// builder/migrator and the two hand-rolled JSON builders that serialize a
// permanent page and the node list into the exact shapes the DigiAsset clients
// expect. See the header for per-method contracts.
//

#include "PoolDatabase.h"
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
    // Escape a string for safe embedding in JSON. Handles the minimum set the
    // pool server's responses actually need (quote, backslash, control chars).
    std::string jsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (char c: s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char) c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    int64_t nowUnix() {
        return (int64_t) std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

// Open (or create) the sqlite file at dbPath, switch it to WAL mode for
// concurrent reads under the HTTP worker writes, and build/migrate the schema.
// Throws std::runtime_error if the database can't be opened.
PoolDatabase::PoolDatabase(const std::string& dbPath) {
    int rc = sqlite3_open(dbPath.c_str(), &_db);
    if (rc != SQLITE_OK) {
        std::string err = "Failed to open pool database " + dbPath + ": " +
                          std::string(sqlite3_errmsg(_db));
        if (_db) sqlite3_close(_db);
        _db = nullptr;
        throw std::runtime_error(err);
    }

    // WAL for concurrent reads with the HTTP server writing on a worker
    // thread. Pool load is tiny but WAL also reduces fsync cost.
    exec("PRAGMA journal_mode = WAL;");
    exec("PRAGMA synchronous = NORMAL;");

    buildSchema();
}

PoolDatabase::~PoolDatabase() {
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

// Run a fire-and-forget SQL statement (used for PRAGMAs and CREATE TABLE).
// Throws std::runtime_error on failure. Caller must already hold the mutex
// where concurrency matters (schema build runs single-threaded at ctor time).
void PoolDatabase::exec(const char* sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = "sqlite exec failed: ";
        if (errMsg) {
            err += errMsg;
            sqlite3_free(errMsg);
        }
        throw std::runtime_error(err);
    }
}

// Create every table (IF NOT EXISTS) and apply the additive column migrations
// for later versions, so opening either a fresh or an existing pool.db leaves
// the schema current. Called once from the constructor.
void PoolDatabase::buildSchema() {
    // Every table uses IF NOT EXISTS so reopening an existing pool.db is a
    // no-op. All columns kept simple — INTEGER for unix times and sizes,
    // TEXT for everything else. No foreign keys; the operator can
    // reason about the tables with plain sqlite3.exe.
    exec(
        "CREATE TABLE IF NOT EXISTS nodes ("
        " peerId        TEXT PRIMARY KEY,"
        " payoutAddress TEXT NOT NULL,"
        " firstSeen     INTEGER NOT NULL,"
        " lastSeen      INTEGER NOT NULL"
        ");"
    );

    // Schema migration: win.33 adds dial-back verification columns. These
    // ALTER TABLEs fail silently if the columns already exist (sqlite's
    // ALTER doesn't have IF NOT EXISTS; we catch the error and move on).
    auto tryAddColumn = [this](const char* sql) {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK && errMsg) {
            // Ignore "duplicate column name" - that just means the migration
            // already ran on a previous launch against this pool.db.
            sqlite3_free(errMsg);
        }
    };
    tryAddColumn("ALTER TABLE nodes ADD COLUMN lastVerifyOk INTEGER NOT NULL DEFAULT 0;");
    tryAddColumn("ALTER TABLE nodes ADD COLUMN verifyFails  INTEGER NOT NULL DEFAULT 0;");
    // win.43 records the node's public IP (seen at registration) so the pool
    // web page can plot a world map of nodes.
    tryAddColumn("ALTER TABLE nodes ADD COLUMN lastAddr TEXT;");
    // win.76 fairness: per-node EMA scores. coverageScore = how much of the
    // permanent CID list the node provably provides (sampled each verify round);
    // reliabilityScore = how consistently it passes verification. Payouts are
    // split proportional to coverageScore * reliabilityScore.
    tryAddColumn("ALTER TABLE nodes ADD COLUMN coverageScore    REAL NOT NULL DEFAULT 0;");
    tryAddColumn("ALTER TABLE nodes ADD COLUMN reliabilityScore REAL NOT NULL DEFAULT 0;");

    // permanent_assets rows are imported from the first-run snapshot of
    // mctrivia's /permanent/<page>.json or added later by the operator.
    // Primary key includes cid because a single assetId can reference
    // multiple CIDs (metadata plus sub-files).
    exec(
        "CREATE TABLE IF NOT EXISTS permanent_assets ("
        " assetId TEXT NOT NULL,"
        " txHash  TEXT NOT NULL,"
        " cid     TEXT NOT NULL,"
        " page    INTEGER NOT NULL,"
        " added   INTEGER NOT NULL,"
        " PRIMARY KEY (assetId, txHash, cid)"
        ");"
    );
    exec(
        "CREATE INDEX IF NOT EXISTS idx_perm_page ON permanent_assets(page);"
    );

    // One row per page. When the operator imports a complete page (done=1)
    // no more entries get added to that page. daily stores the 'daily'
    // string from mctrivia's response so we can serve something plausible.
    exec(
        "CREATE TABLE IF NOT EXISTS permanent_pages ("
        " page   INTEGER PRIMARY KEY,"
        " done   INTEGER NOT NULL DEFAULT 0,"
        " daily  TEXT NOT NULL DEFAULT '0'"
        ");"
    );

    // Payout ledger. Phase 1 never populates this; Phase 3 (operator-
    // approved batch payout) will.
    exec(
        "CREATE TABLE IF NOT EXISTS payouts_ledger ("
        " id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        " payoutAddress TEXT NOT NULL,"
        " amountDgbSat  INTEGER NOT NULL,"
        " owedAt        INTEGER NOT NULL,"
        " paidTxid      TEXT,"
        " paidAt        INTEGER"
        ");"
    );

    exec(
        "CREATE TABLE IF NOT EXISTS pool_config ("
        " key   TEXT PRIMARY KEY,"
        " value TEXT NOT NULL"
        ");"
    );
}

void PoolDatabase::upsertNode(const std::string& peerId,
                              const std::string& payoutAddress,
                              const std::string& observedIp) {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();

    // Use INSERT ... ON CONFLICT to atomic upsert. Keep firstSeen on
    // conflict (preserve the original), update the rest. lastAddr is only
    // overwritten when we actually observed an IP this time: NULLIF(?, '')
    // turns a blank observedIp into NULL, and COALESCE then preserves the
    // previously stored address rather than wiping it.
    const char* sql =
        "INSERT INTO nodes (peerId, payoutAddress, firstSeen, lastSeen, lastAddr) "
        "VALUES (?, ?, ?, ?, NULLIF(?, '')) "
        "ON CONFLICT(peerId) DO UPDATE SET "
        " payoutAddress = excluded.payoutAddress, "
        " lastSeen      = excluded.lastSeen, "
        " lastAddr      = COALESCE(excluded.lastAddr, nodes.lastAddr);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare upsertNode failed: ") +
                                 sqlite3_errmsg(_db));
    }
    sqlite3_bind_text(stmt, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, payoutAddress.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_text(stmt, 5, observedIp.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("upsertNode step failed: ") +
                                 sqlite3_errmsg(_db));
    }
}

void PoolDatabase::recordVerifySuccess(const std::string& peerId) {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    const char* sql =
        "UPDATE nodes SET lastVerifyOk = ?, verifyFails = 0 WHERE peerId = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void PoolDatabase::recordVerifyFailure(const std::string& peerId) {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql =
        "UPDATE nodes SET verifyFails = verifyFails + 1 WHERE peerId = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void PoolDatabase::updateNodeScores(const std::string& peerId, double coverageObs,
                                    bool hasCoverageSample, bool verifyOk) {
    std::lock_guard<std::mutex> lk(_mutex);
    const double A = 0.1;   // EMA smoothing (tunable): recent rounds weigh more
    // Coverage only moves when we had live samples this round; reliability always.
    const char* sql =
        "UPDATE nodes SET "
        " coverageScore    = CASE WHEN ? THEN ? * ? + (1.0 - ?) * coverageScore ELSE coverageScore END, "
        " reliabilityScore = ? * ? + (1.0 - ?) * reliabilityScore "
        "WHERE peerId = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int   (stmt, 1, hasCoverageSample ? 1 : 0);
    sqlite3_bind_double(stmt, 2, A);
    sqlite3_bind_double(stmt, 3, coverageObs);
    sqlite3_bind_double(stmt, 4, A);
    sqlite3_bind_double(stmt, 5, A);
    sqlite3_bind_double(stmt, 6, verifyOk ? 1.0 : 0.0);
    sqlite3_bind_double(stmt, 7, A);
    sqlite3_bind_text  (stmt, 8, peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<std::string> PoolDatabase::getPeerIdsForVerification(unsigned int limit) {
    std::lock_guard<std::mutex> lk(_mutex);
    // Least-recently-verified first. Also favors nodes we've never verified
    // (lastVerifyOk = 0 puts them at the top of the order). Still seen in
    // the last week, so we don't waste time probing ghosts.
    int64_t cutoff = nowUnix() - 7 * 24 * 60 * 60;
    std::vector<std::string> out;
    const char* sql =
        "SELECT peerId FROM nodes WHERE lastSeen >= ? "
        "ORDER BY lastVerifyOk ASC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        sqlite3_bind_int(stmt, 2, (int) limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* p = sqlite3_column_text(stmt, 0);
            if (p) out.emplace_back(reinterpret_cast<const char*>(p));
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

unsigned int PoolDatabase::countVerifiedSince(int64_t unixSeconds) {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(*) FROM nodes WHERE lastVerifyOk >= ?;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, unixSeconds);
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

unsigned int PoolDatabase::countFailedOut() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(*) FROM nodes WHERE verifyFails >= 3;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

std::vector<PoolDatabase::PayoutTarget> PoolDatabase::getVerifiedPayoutTargets() {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    int64_t seenCutoff = now - 7 * 24 * 60 * 60;   // seen in last 7 days
    int64_t verifyCutoff = now - 24 * 60 * 60;       // verified in last 24h

    std::vector<PayoutTarget> out;
    // Eligible AND provably hosting: seen in 7d, verified in 24h, not failed-out,
    // AND coverageScore > 0 (has demonstrably provided sampled content). The
    // payout weight is coverage * reliability, so a partial/flaky host earns less.
    const char* sql =
        "SELECT peerId, payoutAddress, coverageScore, reliabilityScore FROM nodes "
        "WHERE lastSeen >= ? AND lastVerifyOk >= ? AND verifyFails < 3 "
        "AND coverageScore > 0 "
        "ORDER BY peerId;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, seenCutoff);
        sqlite3_bind_int64(stmt, 2, verifyCutoff);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PayoutTarget t;
            const unsigned char* p = sqlite3_column_text(stmt, 0);
            const unsigned char* a = sqlite3_column_text(stmt, 1);
            if (p) t.peerId = (const char*) p;
            if (a) t.payoutAddress = (const char*) a;
            t.coverage = sqlite3_column_double(stmt, 2);
            t.reliability = sqlite3_column_double(stmt, 3);
            t.weight = t.coverage * t.reliability;
            if (!t.payoutAddress.empty() && t.weight > 0.0) out.push_back(t);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void PoolDatabase::recordPayout(const std::string& payoutAddress, int64_t amountDgbSat,
                                const std::string& txid) {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    const char* sql =
        "INSERT INTO payouts_ledger (payoutAddress, amountDgbSat, owedAt, paidTxid, paidAt) "
        "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, payoutAddress.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, amountDgbSat);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_text(stmt, 4, txid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, now);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    if (!ok) {
        // CRITICAL: recordPayout is only called on a REAL txid, so the DGB already
        // left the wallet - but the ledger row failed to write. Without it, the
        // once-per-period guard (getLastPayoutAt) stays stale and could allow a
        // re-pay. Scream so the operator reconciles by hand. (audit M6)
        fprintf(stderr, "[POOL][CRITICAL] payout SENT but LEDGER WRITE FAILED - reconcile manually: addr=%s sat=%lld txid=%s\n",
                payoutAddress.c_str(), (long long) amountDgbSat, txid.c_str());
        fflush(stderr);
    }
}

double PoolDatabase::getPaidTotalDgb() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COALESCE(SUM(amountDgbSat), 0) FROM payouts_ledger WHERE paidTxid IS NOT NULL;";
    sqlite3_stmt* stmt = nullptr;
    double total = 0.0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0) / 100000000.0;
        }
        sqlite3_finalize(stmt);
    }
    return total;
}

unsigned int PoolDatabase::getPaidCount() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(*) FROM payouts_ledger WHERE paidTxid IS NOT NULL;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

int64_t PoolDatabase::getLastPayoutAt() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COALESCE(MAX(paidAt), 0) FROM payouts_ledger WHERE paidTxid IS NOT NULL;";
    sqlite3_stmt* stmt = nullptr;
    int64_t last = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) last = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return last;
}

void PoolDatabase::insertPermanentAsset(const std::string& assetId,
                                        const std::string& txHash,
                                        const std::string& cid,
                                        unsigned int page) {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();

    const char* sql =
        "INSERT OR IGNORE INTO permanent_assets (assetId, txHash, cid, page, added) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare insertPermanentAsset failed: ") +
                                 sqlite3_errmsg(_db));
    }
    sqlite3_bind_text(stmt, 1, assetId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, txHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, cid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, (int) page);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<PoolDatabase::PayoutRow> PoolDatabase::getRecentPayouts(unsigned int limit) {
    std::lock_guard<std::mutex> lk(_mutex);
    std::vector<PayoutRow> out;
    const char* sql =
        "SELECT payoutAddress, amountDgbSat, paidAt, paidTxid FROM payouts_ledger "
        "WHERE paidTxid IS NOT NULL ORDER BY paidAt DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, (int) limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PayoutRow r;
            const unsigned char* a = sqlite3_column_text(stmt, 0);
            if (a) r.payoutAddress = (const char*) a;
            r.amountDgbSat = sqlite3_column_int64(stmt, 1);
            r.paidAt = sqlite3_column_int64(stmt, 2);
            const unsigned char* t = sqlite3_column_text(stmt, 3);
            if (t) r.txid = (const char*) t;
            out.push_back(r);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

std::vector<std::string> PoolDatabase::getActiveNodeIps(int64_t sinceUnix) {
    std::lock_guard<std::mutex> lk(_mutex);
    std::vector<std::string> out;
    const char* sql =
        "SELECT lastAddr FROM nodes "
        "WHERE lastSeen >= ? AND lastAddr IS NOT NULL AND lastAddr <> '' "
        "ORDER BY peerId;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, sinceUnix);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* p = sqlite3_column_text(stmt, 0);
            if (p) out.emplace_back((const char*) p);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

std::vector<std::string> PoolDatabase::getSampleCids(unsigned int limit) {
    std::lock_guard<std::mutex> lk(_mutex);
    std::vector<std::string> out;
    const char* sql =
        "SELECT cid FROM permanent_assets WHERE cid IS NOT NULL AND cid != '' "
        "ORDER BY RANDOM() LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, (int) limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* c = sqlite3_column_text(stmt, 0);
            if (c) out.emplace_back((const char*) c);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void PoolDatabase::setPermanentPageDone(unsigned int page, bool done, const std::string& daily) {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql =
        "INSERT INTO permanent_pages (page, done, daily) VALUES (?, ?, ?) "
        "ON CONFLICT(page) DO UPDATE SET "
        " done  = excluded.done, "
        " daily = excluded.daily;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare setPermanentPageDone failed: ") +
                                 sqlite3_errmsg(_db));
    }
    sqlite3_bind_int(stmt, 1, (int) page);
    sqlite3_bind_int(stmt, 2, done ? 1 : 0);
    sqlite3_bind_text(stmt, 3, daily.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool PoolDatabase::hasPermanentData() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(*) FROM permanent_assets;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count > 0;
}

std::string PoolDatabase::buildPermanentPageJson(unsigned int page) {
    std::lock_guard<std::mutex> lk(_mutex);

    // First pull page metadata (daily + done).
    std::string dailyStr = "0";
    bool done = false;
    {
        const char* sql = "SELECT daily, done FROM permanent_pages WHERE page=?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, (int) page);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* d = sqlite3_column_text(stmt, 0);
                if (d) dailyStr = (const char*) d;
                done = sqlite3_column_int(stmt, 1) != 0;
            }
            sqlite3_finalize(stmt);
        }
    }

    // Then walk the assets. The keys in the "changes" object are formatted
    // as "<assetId>-<txHash>" and the values are JSON arrays of CIDs. A
    // single (assetId, txHash) pair can have multiple CIDs so we group
    // manually instead of using sqlite's json_group_array.
    std::ostringstream json;
    json << "{\"changes\":{";

    const char* sql =
        "SELECT assetId, txHash, cid FROM permanent_assets "
        "WHERE page=? ORDER BY assetId, txHash, cid;";
    sqlite3_stmt* stmt = nullptr;
    bool anyRows = false;
    bool firstKey = true;
    std::string currentKey;
    bool firstCidForKey = true;

    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, (int) page);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            anyRows = true;
            std::string assetId = (const char*) sqlite3_column_text(stmt, 0);
            std::string txHash  = (const char*) sqlite3_column_text(stmt, 1);
            std::string cid     = (const char*) sqlite3_column_text(stmt, 2);
            std::string key     = assetId + "-" + txHash;

            if (key != currentKey) {
                // Close the previous array (if any) and open a new key.
                if (!firstKey) json << "],";
                currentKey = key;
                firstKey = false;
                firstCidForKey = true;
                json << "\"" << jsonEscape(key) << "\":[";
            }
            if (!firstCidForKey) json << ",";
            json << "\"" << jsonEscape(cid) << "\"";
            firstCidForKey = false;
        }
        sqlite3_finalize(stmt);
    }
    if (!firstKey) json << "]"; // close the last value array
    json << "},";
    json << "\"daily\":\"" << jsonEscape(dailyStr) << "\",";
    json << "\"done\":" << (done ? "true" : "false");
    json << "}";

    if (!anyRows) {
        // If the page has no rows AT ALL, mimic mctrivia's server returning
        // an error envelope for pages past the frontier.
        return "{\"error\":\"page " + std::to_string(page) + " not populated\"}";
    }
    return json.str();
}

std::string PoolDatabase::buildNodesJson() {
    std::lock_guard<std::mutex> lk(_mutex);
    // 7-day window, AND exclude nodes that have failed dial-back
    // verification 3+ times in a row. Failed-out nodes are ghosts — they
    // keep sending keepalive pings but the pool operator can't actually
    // reach them via IPFS, so other clients shouldn't see them either.
    int64_t cutoff = nowUnix() - 7 * 24 * 60 * 60;

    const char* sql =
        "SELECT peerId FROM nodes "
        "WHERE lastSeen >= ? AND verifyFails < 3 "
        "ORDER BY lastSeen DESC;";
    sqlite3_stmt* stmt = nullptr;
    std::ostringstream json;
    json << "[";
    bool first = true;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* p = sqlite3_column_text(stmt, 0);
            if (!p) continue;
            if (!first) json << ",";
            first = false;
            json << "{\"id\":\"" << jsonEscape((const char*) p) << "\"}";
        }
        sqlite3_finalize(stmt);
    }
    json << "]";
    return json.str();
}

std::string PoolDatabase::buildMapJson() {
    // For now, return one blank-geo entry per live node so the existing
    // client's node-count heuristic (counting "version" keys in map.json)
    // returns a reasonable number. Real geo resolution is a later phase.
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t cutoff = nowUnix() - 7 * 24 * 60 * 60;

    const char* sql = "SELECT COUNT(*) FROM nodes WHERE lastSeen >= ?;";
    sqlite3_stmt* stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    std::ostringstream json;
    json << "[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json << ",";
        json << "{\"version\":5,\"country\":\"NA\",\"region\":\"NA\",\"city\":\"NA\","
             << "\"longitude\":0,\"latitude\":0}";
    }
    json << "]";
    return json.str();
}

unsigned int PoolDatabase::countNodesSeenSince(int64_t unixSeconds) {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(*) FROM nodes WHERE lastSeen >= ?;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, unixSeconds);
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

unsigned int PoolDatabase::countTotalNodes() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(*) FROM nodes;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

// Count of ACTIVE nodes: seen in the last 7 days AND not failed-out of dial-back
// verification (verifyFails < 3). Matches exactly the set buildNodesJson()
// publishes and the count nodes/the map show, so all surfaces agree.
unsigned int PoolDatabase::countActiveNodes() {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t cutoff = nowUnix() - 7 * 24 * 60 * 60;
    const char* sql = "SELECT COUNT(*) FROM nodes WHERE lastSeen >= ? AND verifyFails < 3;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

unsigned int PoolDatabase::countPermanentAssets() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(DISTINCT assetId) FROM permanent_assets;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

unsigned int PoolDatabase::countPermanentPages() {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT COUNT(*) FROM permanent_pages;";
    sqlite3_stmt* stmt = nullptr;
    unsigned int count = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

void PoolDatabase::setConfig(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql =
        "INSERT INTO pool_config (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string PoolDatabase::getConfig(const std::string& key, const std::string& defaultValue) {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql = "SELECT value FROM pool_config WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    std::string value = defaultValue;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* v = sqlite3_column_text(stmt, 0);
            if (v) value = (const char*) v;
        }
        sqlite3_finalize(stmt);
    }
    return value;
}
