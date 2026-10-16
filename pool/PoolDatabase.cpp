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
#include <map>
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
    // win.77 anti-takeover: the per-node secret bound on first registration.
    tryAddColumn("ALTER TABLE nodes ADD COLUMN secret TEXT NOT NULL DEFAULT '';");
    // Storage plan (pool/StoragePlan.h): proofScore is an EMA of byte-range
    // challenges passed; proofAnswered counts every answer, right or wrong, so
    // the plan can tell a node that proves from one on an older client.
    tryAddColumn("ALTER TABLE nodes ADD COLUMN proofScore    REAL    NOT NULL DEFAULT 0;");
    tryAddColumn("ALTER TABLE nodes ADD COLUMN proofAnswered INTEGER NOT NULL DEFAULT 0;");

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
    // The file's size in bytes, measured by the verifier from the pool's own
    // IPFS (files/stat). 0 = not measured yet. Payouts are weighted by it.
    tryAddColumn("ALTER TABLE permanent_assets ADD COLUMN sizeBytes INTEGER NOT NULL DEFAULT 0;");
    exec(
        "CREATE INDEX IF NOT EXISTS idx_perm_cid ON permanent_assets(cid);"
    );

    // Storage challenges, one row per question asked. answeredAt is set when a
    // node's answer is claimed (or the deadline passes); passed is the verdict,
    // NULL for one closed without an answer from a node never scored on proofs.
    exec(
        "CREATE TABLE IF NOT EXISTS storage_challenges ("
        " id         TEXT PRIMARY KEY,"
        " peerId     TEXT NOT NULL,"
        " cid        TEXT NOT NULL,"
        " offset     INTEGER NOT NULL,"
        " length     INTEGER NOT NULL,"
        " issuedAt   INTEGER NOT NULL,"
        " answeredAt INTEGER,"
        " passed     INTEGER"
        ");"
    );
    exec(
        "CREATE INDEX IF NOT EXISTS idx_challenge_open ON storage_challenges(peerId, answeredAt);"
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
                              const std::string& secret,
                              const std::string& observedIp) {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();

    // Look up whether this peerId is already bound, and to which secret.
    std::string storedSecret;
    bool exists = false;
    {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(_db, "SELECT secret FROM nodes WHERE peerId = ?;", -1, &q, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(q) == SQLITE_ROW) {
                exists = true;
                const unsigned char* s = sqlite3_column_text(q, 0);
                if (s) storedSecret = (const char*) s;
            }
            sqlite3_finalize(q);
        }
    }

    if (!exists) {
        // First registration: bind peerId -> secret + payoutAddress.
        sqlite3_stmt* stmt = nullptr;
        const char* ins =
            "INSERT INTO nodes (peerId, payoutAddress, secret, firstSeen, lastSeen, lastAddr) "
            "VALUES (?, ?, ?, ?, ?, NULLIF(?, ''));";
        if (sqlite3_prepare_v2(_db, ins, -1, &stmt, nullptr) != SQLITE_OK) return;
        sqlite3_bind_text(stmt, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, payoutAddress.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, secret.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_int64(stmt, 5, now);
        sqlite3_bind_text(stmt, 6, observedIp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return;
    }

    // Existing node. A payoutAddress change is only honored if the secret matches
    // (or the row predates secrets - stored empty - in which case we adopt the
    // first one we see). A mismatched secret is rejected ENTIRELY so an attacker
    // can't touch a victim's row at all. (audit M1)
    bool secretOk = storedSecret.empty() || (!secret.empty() && secret == storedSecret);
    if (!secretOk) {
        fprintf(stderr, "[POOL] rejected registration for peerId=%s (secret mismatch) - possible payout-address takeover attempt\n", peerId.c_str());
        fflush(stderr);
        return;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* upd =
        "UPDATE nodes SET payoutAddress = ?, "
        " secret = CASE WHEN secret = '' THEN ? ELSE secret END, "
        " lastSeen = ?, lastAddr = COALESCE(NULLIF(?, ''), lastAddr) "
        "WHERE peerId = ?;";
    if (sqlite3_prepare_v2(_db, upd, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, payoutAddress.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, secret.c_str(), -1, SQLITE_TRANSIENT);   // adopt if previously empty
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_text(stmt, 4, observedIp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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

std::vector<StoragePlan::Node> PoolDatabase::getPlanNodes() {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    std::vector<StoragePlan::Node> out;
    // The same eligibility gate payouts always had: seen in 7 days, verified
    // (reachable) in 24 hours, fewer than 3 consecutive failures. Whether a
    // node HOLDS anything is the plan's question, not this query's.
    const char* sql =
        "SELECT peerId, reliabilityScore, coverageScore, proofScore, proofAnswered, firstSeen FROM nodes "
        "WHERE lastSeen >= ? AND lastVerifyOk >= ? AND verifyFails < 3 "
        "ORDER BY peerId;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now - 7 * 24 * 60 * 60);
        sqlite3_bind_int64(stmt, 2, now - 24 * 60 * 60);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            StoragePlan::Node n;
            const unsigned char* p = sqlite3_column_text(stmt, 0);
            if (p) n.peerId = (const char*) p;
            n.reliability = sqlite3_column_double(stmt, 1);
            n.coverage = sqlite3_column_double(stmt, 2);
            n.proofScore = sqlite3_column_double(stmt, 3);
            n.proofAnswered = sqlite3_column_int64(stmt, 4);
            n.firstSeen = sqlite3_column_int64(stmt, 5);
            if (!n.peerId.empty()) out.push_back(n);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

std::vector<StoragePlan::Cid> PoolDatabase::getPlanCids() {
    std::lock_guard<std::mutex> lk(_mutex);
    std::vector<StoragePlan::Cid> out;
    // Distinct CIDs: one CID can sit on several rows (assetId/txHash pairs),
    // and it is one file to store, so it is one file to pay for.
    const char* sql =
        "SELECT cid, MAX(sizeBytes) FROM permanent_assets WHERE cid IS NOT NULL AND cid != '' "
        "GROUP BY cid ORDER BY cid;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            StoragePlan::Cid c;
            const unsigned char* p = sqlite3_column_text(stmt, 0);
            if (p) c.cid = (const char*) p;
            c.sizeBytes = sqlite3_column_int64(stmt, 1);
            out.push_back(c);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

std::vector<std::string> PoolDatabase::getCidsMissingSize(unsigned int limit) {
    std::lock_guard<std::mutex> lk(_mutex);
    std::vector<std::string> out;
    const char* sql =
        "SELECT cid FROM permanent_assets WHERE cid IS NOT NULL AND cid != '' "
        "GROUP BY cid HAVING MAX(sizeBytes) = 0 ORDER BY RANDOM() LIMIT ?;";
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

void PoolDatabase::setCidSize(const std::string& cid, int64_t sizeBytes) {
    if (sizeBytes <= 0) return;
    std::lock_guard<std::mutex> lk(_mutex);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, "UPDATE permanent_assets SET sizeBytes = ? WHERE cid = ?;", -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, sizeBytes);
    sqlite3_bind_text(stmt, 2, cid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<PoolDatabase::PayoutTarget> PoolDatabase::getVerifiedPayoutTargets(const StoragePlan::Config& plan) {
    // Inputs first, each under its own lock; the plan itself is pure.
    auto nodes = getPlanNodes();
    auto cids = getPlanCids();
    auto weights = StoragePlan::weights(nodes, cids, plan);

    std::map<std::string, std::string> addressOf;
    std::map<std::string, StoragePlan::Node> nodeOf;
    for (const auto& n: nodes) nodeOf[n.peerId] = n;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, "SELECT peerId, payoutAddress FROM nodes;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* p = sqlite3_column_text(stmt, 0);
                const unsigned char* a = sqlite3_column_text(stmt, 1);
                if (p && a) addressOf[(const char*) p] = (const char*) a;
            }
            sqlite3_finalize(stmt);
        }
    }

    std::vector<PayoutTarget> out;
    for (const auto& w: weights) {
        PayoutTarget t;
        t.peerId = w.peerId;
        t.payoutAddress = addressOf.count(w.peerId) ? addressOf[w.peerId] : "";
        t.coverage = nodeOf[w.peerId].coverage;
        t.reliability = nodeOf[w.peerId].reliability;
        t.weight = w.weight;
        t.assignedBytes = w.assignedBytes;
        t.assignedCids = w.assignedCids;
        t.held = w.held;
        t.legacy = w.legacy;
        if (!t.payoutAddress.empty() && t.weight > 0.0) out.push_back(t);
    }
    return out;
}

bool PoolDatabase::nodeSecretMatches(const std::string& peerId, const std::string& secret) {
    if (secret.empty()) return false;
    std::lock_guard<std::mutex> lk(_mutex);
    bool ok = false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, "SELECT secret FROM nodes WHERE peerId = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* s = sqlite3_column_text(stmt, 0);
            ok = s && secret == (const char*) s;
        }
        sqlite3_finalize(stmt);
    }
    return ok;
}

namespace {
    bool readChallenge(sqlite3_stmt* stmt, PoolDatabase::Challenge& out) {
        const unsigned char* id = sqlite3_column_text(stmt, 0);
        const unsigned char* peer = sqlite3_column_text(stmt, 1);
        const unsigned char* cid = sqlite3_column_text(stmt, 2);
        if (!id || !peer || !cid) return false;
        out.id = (const char*) id;
        out.peerId = (const char*) peer;
        out.cid = (const char*) cid;
        out.offset = sqlite3_column_int64(stmt, 3);
        out.length = sqlite3_column_int64(stmt, 4);
        out.issuedAt = sqlite3_column_int64(stmt, 5);
        return true;
    }
}

bool PoolDatabase::getOpenChallenge(const std::string& peerId, Challenge& out) {
    std::lock_guard<std::mutex> lk(_mutex);
    bool found = false;
    const char* sql =
        "SELECT id, peerId, cid, offset, length, issuedAt FROM storage_challenges "
        "WHERE peerId = ? AND answeredAt IS NULL AND issuedAt >= ? ORDER BY issuedAt DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, peerId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, nowUnix() - StoragePlan::CHALLENGE_DEADLINE_SECONDS);
        if (sqlite3_step(stmt) == SQLITE_ROW) found = readChallenge(stmt, out);
        sqlite3_finalize(stmt);
    }
    return found;
}

void PoolDatabase::insertChallenge(const Challenge& c) {
    std::lock_guard<std::mutex> lk(_mutex);
    const char* sql =
        "INSERT INTO storage_challenges (id, peerId, cid, offset, length, issuedAt) VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, c.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, c.peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, c.cid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, c.offset);
    sqlite3_bind_int64(stmt, 5, c.length);
    sqlite3_bind_int64(stmt, 6, c.issuedAt);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool PoolDatabase::claimChallenge(const std::string& id, const std::string& peerId, Challenge& out) {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    // Read and claim under one lock: the UPDATE only lands on a row still open
    // and inside its deadline, so a challenge is answered at most once.
    bool found = false;
    {
        const char* sql =
            "SELECT id, peerId, cid, offset, length, issuedAt FROM storage_challenges "
            "WHERE id = ? AND peerId = ? AND answeredAt IS NULL AND issuedAt >= ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, peerId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 3, now - StoragePlan::CHALLENGE_DEADLINE_SECONDS);
            if (sqlite3_step(stmt) == SQLITE_ROW) found = readChallenge(stmt, out);
            sqlite3_finalize(stmt);
        }
    }
    if (!found) return false;
    sqlite3_stmt* upd = nullptr;
    if (sqlite3_prepare_v2(_db, "UPDATE storage_challenges SET answeredAt = ? WHERE id = ? AND answeredAt IS NULL;",
                           -1, &upd, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(upd, 1, now);
    sqlite3_bind_text(upd, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    bool claimed = sqlite3_step(upd) == SQLITE_DONE && sqlite3_changes(_db) == 1;
    sqlite3_finalize(upd);
    return claimed;
}

void PoolDatabase::recordProofResult(const std::string& id, const std::string& peerId, bool passed) {
    std::lock_guard<std::mutex> lk(_mutex);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, "UPDATE storage_challenges SET passed = ? WHERE id = ? AND peerId = ? AND passed IS NULL;",
                           -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, passed ? 1 : 0);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, peerId.c_str(), -1, SQLITE_TRANSIENT);
    bool recorded = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(_db) == 1;
    sqlite3_finalize(stmt);
    if (!recorded) return;   // a verdict is folded in once

    sqlite3_stmt* node = nullptr;
    const char* sql =
        "UPDATE nodes SET proofScore = ? * ? + (1.0 - ?) * proofScore, proofAnswered = proofAnswered + 1 "
        "WHERE peerId = ?;";
    if (sqlite3_prepare_v2(_db, sql, -1, &node, nullptr) != SQLITE_OK) return;
    sqlite3_bind_double(node, 1, StoragePlan::PROOF_EMA);
    sqlite3_bind_double(node, 2, passed ? 1.0 : 0.0);
    sqlite3_bind_double(node, 3, StoragePlan::PROOF_EMA);
    sqlite3_bind_text(node, 4, peerId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(node);
    sqlite3_finalize(node);
}

unsigned int PoolDatabase::expireChallenges() {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    int64_t cutoff = now - StoragePlan::CHALLENGE_DEADLINE_SECONDS;
    // A node that has answered before and ignored this one is scored a fail,
    // so going quiet when a challenge lands on a file you dropped does not
    // protect your score.
    const char* penalise =
        "UPDATE nodes SET proofScore = (1.0 - ?) * proofScore, proofAnswered = proofAnswered + 1 "
        "WHERE proofAnswered > 0 AND peerId IN ("
        " SELECT peerId FROM storage_challenges WHERE answeredAt IS NULL AND issuedAt < ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, penalise, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_double(stmt, 1, StoragePlan::PROOF_EMA);
        sqlite3_bind_int64(stmt, 2, cutoff);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    const char* close =
        "UPDATE storage_challenges SET answeredAt = ?, "
        " passed = CASE WHEN peerId IN (SELECT peerId FROM nodes WHERE proofAnswered > 0) THEN 0 ELSE NULL END "
        "WHERE answeredAt IS NULL AND issuedAt < ?;";
    unsigned int closed = 0;
    if (sqlite3_prepare_v2(_db, close, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_int64(stmt, 2, cutoff);
        if (sqlite3_step(stmt) == SQLITE_DONE) closed = (unsigned int) sqlite3_changes(_db);
        sqlite3_finalize(stmt);
    }
    return closed;
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

void PoolDatabase::recordPendingPayout(const std::string& payoutAddress, int64_t amountDgbSat) {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    // paidTxid NULL = attempted-but-unconfirmed: excluded from paid totals, but
    // its owedAt still advances the once-per-period guard (getLastPayoutAt). (M7)
    const char* sql =
        "INSERT INTO payouts_ledger (payoutAddress, amountDgbSat, owedAt, paidTxid, paidAt) "
        "VALUES (?, ?, ?, NULL, NULL);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, payoutAddress.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, amountDgbSat);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int64_t PoolDatabase::beginPayoutPeriod() {
    std::lock_guard<std::mutex> lk(_mutex);
    int64_t now = nowUnix();
    // Durable once-per-period guard written BEFORE the payout broadcast. It
    // advances getLastPayoutAt() (MAX owedAt) immediately, so even if the tx
    // broadcasts but the per-recipient ledger rows fail to write - or we crash
    // between broadcast and record - a re-run is still blocked and we can't
    // double-pay real money. paidTxid/paidAt NULL keeps this sentinel out of
    // every paid total, the payout count, and the public recent-payouts list
    // (all of which filter paidTxid IS NOT NULL). Returns the row id, or -1 on
    // failure. (B-POOL4)
    const char* sql =
        "INSERT INTO payouts_ledger (payoutAddress, amountDgbSat, owedAt, paidTxid, paidAt) "
        "VALUES ('__PERIOD_GUARD__', 0, ?, NULL, NULL);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, now);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok) return -1;
    return sqlite3_last_insert_rowid(_db);
}

void PoolDatabase::abortPayoutPeriod(int64_t guardId) {
    if (guardId < 0) return;
    std::lock_guard<std::mutex> lk(_mutex);
    // Remove the pre-broadcast guard ONLY when the send cleanly failed (nothing
    // left the wallet), so a failed attempt doesn't consume the payout period. (B-POOL4)
    const char* sql = "DELETE FROM payouts_ledger WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, guardId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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
    // Count ANY attempt (paid OR pending/ambiguous), so an ambiguous send blocks
    // an immediate re-run for the period - not just confirmed payouts. (audit M7)
    const char* sql = "SELECT COALESCE(MAX(owedAt), 0) FROM payouts_ledger;";
    sqlite3_stmt* stmt = nullptr;
    int64_t last = 0;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) last = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return last;
}

unsigned int PoolDatabase::removePermanentCids(const std::vector<std::string>& cids) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (cids.empty()) return 0;

    // One statement, re-bound per CID, rather than an IN (...) list built by
    // string concatenation. The values come from an HTTP body, so they are not
    // going anywhere near the SQL text.
    const char* sql = "DELETE FROM permanent_assets WHERE cid = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare removePermanentCids failed: ") +
                                 sqlite3_errmsg(_db));
    }

    unsigned int removed = 0;
    for (const auto& cid : cids) {
        if (cid.empty()) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            removed += (unsigned int) sqlite3_changes(_db);
        }
    }
    sqlite3_finalize(stmt);
    return removed;
}

unsigned int PoolDatabase::removeMalformedPermanentCids() {
    std::lock_guard<std::mutex> lk(_mutex);

    // GLOB, not LIKE: GLOB is case-sensitive and supports the [^...] negated
    // class, so this says "contains at least one character that is not a
    // letter or a digit". Every real CID is base58btc or base32 and so is
    // alphanumeric throughout; a URI or a path is not. There is no wildcard
    // here that a valid CID could trip.
    const char* sql =
        "DELETE FROM permanent_assets WHERE cid GLOB '*[^A-Za-z0-9]*';";

    char* err = nullptr;
    if (sqlite3_exec(_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string message = err ? err : "unknown error";
        if (err) sqlite3_free(err);
        throw std::runtime_error("removeMalformedPermanentCids failed: " + message);
    }
    return (unsigned int) sqlite3_changes(_db);
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

std::vector<PoolDatabase::PermanentAssetRow> PoolDatabase::getAllPermanentAssets() {
    std::lock_guard<std::mutex> lk(_mutex);
    std::vector<PermanentAssetRow> out;
    const char* sql = "SELECT assetId, txHash, cid FROM permanent_assets;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PermanentAssetRow r;
            const unsigned char* a = sqlite3_column_text(stmt, 0);
            if (a) r.assetId = (const char*) a;
            const unsigned char* t = sqlite3_column_text(stmt, 1);
            if (t) r.txHash = (const char*) t;
            const unsigned char* c = sqlite3_column_text(stmt, 2);
            if (c) r.cid = (const char*) c;
            out.push_back(r);
        }
        sqlite3_finalize(stmt);
    }
    return out;
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

unsigned int PoolDatabase::getWritablePage() {
    std::lock_guard<std::mutex> lk(_mutex);

    // Highest page that currently holds assets (-1 when the pool is empty).
    long long maxPage = -1;
    {
        const char* sql = "SELECT COALESCE(MAX(page), -1) FROM permanent_assets;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) maxPage = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    // Empty pool: start at the stock client default (psp1permanentpage=23) so a
    // default client, which begins its walk there, reaches our entries.
    if (maxPage < 0) return 23;

    // If that page has been finalized (done=true), open the next one.
    bool done = false;
    {
        const char* sql = "SELECT done FROM permanent_pages WHERE page=?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, (int) maxPage);
            if (sqlite3_step(stmt) == SQLITE_ROW) done = sqlite3_column_int(stmt, 0) != 0;
            sqlite3_finalize(stmt);
        }
    }
    if (done) return (unsigned int) (maxPage + 1);

    // Keep pages a reasonable size: once the open page fills, mark it done so
    // clients advance past it, and write new entries to the next page.
    const unsigned int PAGE_CAP = 500;
    unsigned int count = 0;
    {
        const char* sql = "SELECT COUNT(*) FROM permanent_assets WHERE page=?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, (int) maxPage);
            if (sqlite3_step(stmt) == SQLITE_ROW) count = (unsigned int) sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    if (count >= PAGE_CAP) {
        // Finalize the full page inline (the mutex is already held here, so we
        // can't call setPermanentPageDone which would re-lock).
        const char* sql =
            "INSERT INTO permanent_pages (page, done, daily) VALUES (?, 1, '0') "
            "ON CONFLICT(page) DO UPDATE SET done=1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, (int) maxPage);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return (unsigned int) (maxPage + 1);
    }
    return (unsigned int) maxPage;
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
