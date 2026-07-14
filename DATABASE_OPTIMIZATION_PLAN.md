# DigiAsset Core - Database Optimization Plan

## Current State Summary

- **Engine**: SQLite3 (system/vcpkg), single file `chain.db`
- **Schema version**: 6, with lambda-based migration system
- **Tables**: 15 core + dynamic stats tables
- **Prepared statements**: 164+ cached as class members
- **Threading**: `SQLITE_OPEN_FULLMUTEX` + per-statement `std::mutex` + RAII `LockedStatement`
- **Journaling**: `PRAGMA journal_mode = MEMORY` + `PRAGMA synchronous = OFF` (when write verification disabled)
- **Caching**: In-memory caches for flags, exchange watch addresses, domain masters, IPFS pause state
- **Connection**: Single `sqlite3*` instance, no pooling
- **Profiling**: Built-in per-statement lock duration and count tracking

---

## Phase 1: PRAGMA & Journal Mode Tuning (Low Risk, High Impact)

### 1A. Switch to WAL (Write-Ahead Logging) Mode

**Current**: `journal_mode = MEMORY` (set in `disableWriteVerification()`)

**Proposed**: `PRAGMA journal_mode = WAL;`

WAL allows concurrent readers and a single writer without blocking each other. The current MEMORY journal serializes all access and loses data on crash.

**Pros**:
- Readers never block writers, writers never block readers - major concurrency win for RPC reads during chain sync
- Crash-safe (unlike MEMORY journal which loses data on power failure)
- Typically faster for workloads with mixed reads and writes (which this is)
- WAL files are auto-checkpointed; SQLite manages them

**Cons**:
- WAL file (`chain.db-wal`) and shared-memory file (`chain.db-shm`) appear alongside the database - user must back up all three files together (or run `PRAGMA wal_checkpoint(TRUNCATE)` before backup)
- Slightly slower for write-heavy-only workloads (rare in this app after initial sync)
- Not supported on network filesystems (NFS) - shouldn't matter for local use

**Migration**: No schema change. Just execute the PRAGMA on open. WAL mode persists in the database file so only needs to be set once. Add a migration step that sets it and updates a flag.

### 1B. Tune synchronous to NORMAL Under WAL

**Current**: `synchronous = OFF`

**Proposed**: `PRAGMA synchronous = NORMAL;` (when WAL is active)

Under WAL mode, `NORMAL` is the sweet spot - it syncs at checkpoints but not on every commit. Almost as fast as `OFF` but with crash protection.

**Pros**:
- Near-zero performance difference vs `OFF` under WAL
- Survives application crashes (data only at risk during OS crash or power loss, and even then only the last transaction)
- Eliminates need for "recheck database at startup" logic currently noted in code

**Cons**:
- Microscopically slower than `OFF` for individual commits (unmeasurable in practice)

**Migration**: None. Runtime PRAGMA change.

### 1C. Increase Cache Size

**Current**: SQLite default (~2MB / ~500 pages)

**Proposed**: `PRAGMA cache_size = -65536;` (64MB, negative means KB)

The database holds blockchain data that grows large. A bigger page cache means fewer disk reads for repeated queries.

**Pros**:
- Reduces disk I/O significantly for repeated queries (asset lookups, UTXO scans)
- Directly speeds up the hot-path RPC queries that hit the same tables
- 64MB is modest for a blockchain node

**Cons**:
- Uses more RAM (~64MB)
- Must be set on every connection open (not persisted)

**Migration**: None. Runtime PRAGMA.

### 1D. Enable Memory-Mapped I/O

**Proposed**: `PRAGMA mmap_size = 268435456;` (256MB)

Lets SQLite use the OS page cache directly via mmap instead of copying data through its own buffers.

**Pros**:
- Can dramatically speed up read-heavy workloads
- Works well with WAL mode
- OS manages the mapping efficiently
- Cross-platform (works on macOS, Linux, Windows)

**Cons**:
- Uses virtual address space (not a problem on 64-bit systems)
- On 32-bit systems, could be problematic (set lower or skip)
- Slight risk with I/O errors being turned into SIGBUS - but this is a local database, not networked

**Migration**: None. Runtime PRAGMA.

---

## Phase 2: Schema & Index Optimization (Medium Risk, High Impact)

### 2A. Add Covering Indexes for Hot Queries

Several high-frequency queries scan more data than necessary. Covering indexes let SQLite answer queries entirely from the index without touching the main table.

**Proposed new indexes**:

```sql
-- For getAddressHoldings: SELECT assetIndex,SUM(amount) ... WHERE heightDestroyed IS NULL AND address=?
CREATE INDEX idx_utxos_addr_alive ON utxos(address, heightDestroyed, assetIndex, amount);

-- For getAssetHolders: SELECT address,SUM(amount) ... WHERE assetIndex=? AND heightDestroyed IS NULL
CREATE INDEX idx_utxos_asset_alive ON utxos(assetIndex, heightDestroyed, address, amount);

-- For spendUTXO: UPDATE utxos SET heightDestroyed=? WHERE txid=? AND vout=?
-- (idx_utxos_txid_vout already exists, this is fine)

-- For getValidUTXO (wallet scanning)
CREATE INDEX idx_utxos_addr_valid ON utxos(address, heightDestroyed, heightCreated, txid, vout, aout, assetIndex, amount);
```

**Pros**:
- `getAddressHoldings` and `getAssetHolders` are among the most frequent RPC calls - covering indexes avoid table lookups entirely
- Indexes are created once and maintained automatically
- Can be added with `CREATE INDEX IF NOT EXISTS` (no migration risk)

**Cons**:
- Each index adds disk space (estimate: 10-30% increase in DB size depending on UTXO count)
- Slightly slows down UTXO inserts/updates (but these are already batched in transactions)
- Need to verify via `EXPLAIN QUERY PLAN` that SQLite actually uses them

**Migration**: Add via `CREATE INDEX IF NOT EXISTS` in `initializeClassValues()` (same pattern already used for performance indexes). No schema version bump needed.

### 2B. Consider WITHOUT ROWID on Lookup Tables

Tables like `exchangeWatch`, `flags`, `kyc`, `encryptedkeys`, and `domainsMasters` are small lookup tables with TEXT primary keys. `WITHOUT ROWID` stores them as a clustered index, which is more efficient for TEXT-keyed tables.

**Pros**:
- ~2x faster lookups on TEXT-keyed tables
- Smaller storage footprint (no hidden rowid column)

**Cons**:
- Requires table recreation (DROP + CREATE)
- Need migration with data copy
- Cannot be used on tables with AUTOINCREMENT (`assets`, `ipfs` are excluded)
- Marginal benefit since these tables are small

**Migration**: Would require a new version (v6 -> v7) with data migration. Low priority given the small table sizes.

### 2C. UTXO Table Partitioning via Separate Spent Table

The `utxos` table is the largest and most queried. Currently, spent and unspent UTXOs live together, meaning queries for live UTXOs (`WHERE heightDestroyed IS NULL`) must skip over all spent rows.

**Proposed**: Split into two tables:
- `utxos_live` - only unspent UTXOs (no `heightDestroyed` or `spentTXID` columns)
- `utxos_spent` - spent UTXOs moved here with full history

**Pros**:
- `utxos_live` table stays small (only current UTXO set) - dramatically faster for all "current state" queries
- `utxos_spent` can be aggressively pruned without affecting live queries
- Index sizes shrink proportionally
- The most impactful single optimization for a mature blockchain

**Cons**:
- Significant code change: every query touching utxos needs review
- `spendUTXO()` becomes a DELETE+INSERT across tables instead of UPDATE
- Queries that join live and historical data become UNION queries
- Need careful migration: move existing data row-by-row
- Two tables to maintain instead of one
- Rollback (reorg handling) becomes: DELETE from spent, INSERT back to live

**Migration**: Version 7. Transaction-wrapped migration:
1. Create `utxos_live` and `utxos_spent`
2. INSERT INTO `utxos_live` SELECT ... FROM utxos WHERE heightDestroyed IS NULL
3. INSERT INTO `utxos_spent` SELECT ... FROM utxos WHERE heightDestroyed IS NOT NULL
4. DROP TABLE utxos
5. Recreate indexes on both tables

---

## Phase 3: Connection & Concurrency (Medium Risk, Medium Impact)

### 3A. Read-Only Connection Pool for RPC Queries

**Current**: Single `sqlite3*` connection shared across all threads with per-statement mutexes.

**Proposed**: Open 2-4 additional read-only connections (`SQLITE_OPEN_READONLY`) for RPC query handling, while keeping the single write connection.

With WAL mode (Phase 1A), readers don't block the writer and vice versa. But currently, per-statement mutexes serialize even read queries against each other.

**Pros**:
- RPC queries can execute truly in parallel instead of serializing on statement mutexes
- Writer (ChainAnalyzer) is never blocked by RPC reads
- Simple implementation: each read connection gets its own set of prepared statements

**Cons**:
- Each connection uses its own memory for page cache (mitigated by shared cache mode or mmap)
- Need to manage connection lifecycle
- Prepared statements can't be shared across connections - need separate statement sets per connection
- Adds complexity to Database class
- Need to ensure read connections see consistent state (WAL handles this naturally)

**Migration**: Code-only change. No schema change. Could use a simple round-robin or thread-local connection assignment.

### 3B. Replace Per-Statement Mutexes with Connection-Level Locking

If read connections are separate (3A), the write connection only needs a single mutex instead of per-statement mutexes. Read connections under WAL don't need locking against each other at all (SQLite handles it internally).

**Pros**:
- Simplifies locking model dramatically
- Removes overhead of 164+ individual mutexes
- Less risk of deadlock from multiple mutex acquisitions

**Cons**:
- Only beneficial after 3A is implemented
- Write operations become fully serialized (but they already effectively are due to SQLite's single-writer nature)
- Requires reworking `LockedStatement` pattern

**Migration**: Code refactor only.

---

## Phase 4: Query Optimization (Low Risk, Medium Impact)

### 4A. Fix N+1 Query Patterns

Several RPC methods (identified in `listassets`, `getaddressholdings` response building) loop over results and issue individual queries per row.

**Example** (listassets):
```cpp
for (const auto& asset : assets) {
    db->isAssetInPool(...);   // 1 query per asset
    db->getAsset(...);         // 1 more query per asset
}
```

**Proposed**: Add batch query methods that accept vectors of IDs:
```cpp
// New method
std::map<int, DigiAsset> getAssets(const std::vector<int>& assetIndexes);
```

Or use `WHERE assetIndex IN (?, ?, ...)` with dynamically constructed statements.

**Pros**:
- Reduces round-trips from N to 1
- SQLite handles IN-lists efficiently
- Can be done incrementally per method

**Cons**:
- Dynamic SQL with variable-length IN-lists can't be pre-prepared (need to construct at runtime)
- Alternative: use temporary tables for large batches
- Need to add new methods alongside existing ones

**Migration**: Code-only. Add new methods, update call sites.

### 4B. Optimize Complex UNION Queries

`getAddressTxHistory` and `getAssetTxHistory` use UNION queries that scan large portions of the utxos table. These can be slow for addresses/assets with many transactions.

**Proposed**:
- Add composite indexes specifically for these queries (partially covered by 2A)
- Consider materialized views (as a separate table maintained on insert) for frequently requested aggregations
- Add pagination support where not already present

**Pros**:
- Directly addresses slowest RPC queries
- Pagination prevents unbounded result sets

**Cons**:
- Materialized views require maintenance code on every write
- Adds storage overhead
- Risk of inconsistency if update logic has bugs

**Migration**: Index changes via Phase 2A pattern. Materialized views would need schema version bump.

### 4C. Replace Text-Based Flag Lookups with Integer Keys

`_stmtCheckFlag` uses `WHERE key LIKE ?` on text keys. Integer comparisons are faster.

**Proposed**: Add an integer `flagId` column or use an enum-to-int mapping in code, keeping the text key for human readability.

**Pros**:
- Integer comparison is ~5x faster than text LIKE
- Flags are checked frequently

**Cons**:
- Minimal real-world impact since flags table is tiny and results are cached in `_flagState`
- Added complexity for near-zero gain given the cache
- Low priority

**Migration**: Not worth the migration cost given the in-memory cache.

---

## Phase 5: Data Management (Low Risk, Low-Medium Impact)

### 5A. Auto-VACUUM or Incremental VACUUM

**Current**: No VACUUM configured. As UTXOs are created and pruned, the database file grows but never shrinks.

**Proposed**: `PRAGMA auto_vacuum = INCREMENTAL;` with periodic `PRAGMA incremental_vacuum(N);`

**Pros**:
- Prevents database file bloat over time
- Incremental mode avoids the full-lock of `VACUUM`
- Can be run during idle periods

**Cons**:
- `auto_vacuum` must be set before any tables are created (or requires full VACUUM to switch)
- Slightly slows down deletes (pages are reorganized)
- For existing databases, requires: backup, recreate with auto_vacuum, restore

**Migration**: For new databases: set PRAGMA before table creation. For existing: would need a one-time full VACUUM or database rebuild. Could be triggered during the v7 migration.

### 5B. Smarter Pruning Strategy

**Current**: Pruning deletes rows with `heightDestroyed < threshold`. This can cause long locks during large deletes.

**Proposed**: Batch deletes with LIMIT:
```sql
DELETE FROM utxos WHERE heightDestroyed < ? LIMIT 10000;
-- Repeat until 0 rows affected
```

**Pros**:
- Prevents long write locks that block readers
- Smoother performance during pruning
- Can yield between batches to let other operations through

**Cons**:
- SQLite doesn't support `DELETE ... LIMIT` by default - requires compile-time `SQLITE_ENABLE_UPDATE_DELETE_LIMIT`
- Alternative: use a subquery `DELETE FROM utxos WHERE rowid IN (SELECT rowid FROM utxos WHERE heightDestroyed < ? LIMIT 10000)`
- Slightly more code complexity

**Migration**: Code-only change.

### 5C. BLOB Storage Optimization for txid

Transaction IDs (`txid`) are stored as BLOB (32 bytes). The composite primary key `(address TEXT, txid BLOB, vout INT, aout INT)` on utxos is wide, which makes every secondary index large because SQLite copies the full PK into every index entry.

**Proposed (long-term)**: Add a `utxoId INTEGER PRIMARY KEY` and demote the current PK to a UNIQUE constraint. Secondary indexes then store only the integer rowid.

**Pros**:
- Every secondary index shrinks significantly (8-byte rowid vs ~70+ byte composite key)
- Faster index scans, less I/O
- Standard SQLite optimization pattern

**Cons**:
- Requires updating all code that inserts/queries UTXOs
- Migration must rewrite the entire utxos table
- Adds one level of indirection for lookups by (address, txid, vout, aout)

**Migration**: Version 7. High effort but high long-term benefit.

---

## Phase 6: Stability & Resilience (Medium Risk, High Impact)

### 6A. Integrity Check on Startup

**Current**: If `synchronous = OFF` was active during a crash, data may be corrupt with no detection.

**Proposed**: Run `PRAGMA quick_check;` on startup (fast) or `PRAGMA integrity_check;` (thorough, slower) after unclean shutdown.

Track clean shutdown with a flag:
```cpp
// On startup: check if "cleanShutdown" flag is 1
// If 0: run integrity check
// On startup: set flag to 0
// On clean shutdown: set flag to 1
```

**Pros**:
- Catches corruption before it propagates
- With WAL+NORMAL sync (Phase 1), corruption risk drops dramatically, making this a safety net
- Quick check is fast even on large databases

**Cons**:
- `integrity_check` can take minutes on large databases
- `quick_check` is faster but less thorough
- If corruption is found, only option is reset/re-sync anyway

**Migration**: Code-only. Add flag to `flags` table.

### 6B. Proper Error Handling in Transaction Management

**Current**: `startTransaction()` and `endTransaction()` ignore errors from `sqlite3_exec`:
```cpp
void Database::startTransaction() {
    char* zErrMsg = nullptr;
    sqlite3_exec(_db, "BEGIN TRANSACTION", nullptr, nullptr, &zErrMsg);
    // zErrMsg is never checked or freed
}
```

**Proposed**: Check return codes, free error messages, and propagate failures:
```cpp
void Database::startTransaction() {
    char* zErrMsg = nullptr;
    int rc = sqlite3_exec(_db, "BEGIN TRANSACTION", nullptr, nullptr, &zErrMsg);
    if (rc != SQLITE_OK) {
        std::string err = zErrMsg ? zErrMsg : "unknown error";
        sqlite3_free(zErrMsg);
        throw exceptionFailedSQLCommand(); // or new specific exception
    }
}
```

**Pros**:
- Prevents silent failures that could lead to data inconsistency
- Memory leak fix (zErrMsg is currently leaked on error)
- Follows established error handling patterns used elsewhere in the codebase

**Cons**:
- Callers must handle the exception (but they should already be prepared)
- Minimal risk since this is a correctness fix

**Migration**: Code-only.

### 6C. Retry Logic Improvements

**Current**: `executeSqliteStepWithRetry` retries 3 times with 100ms sleep for `SQLITE_LOCKED`/`SQLITE_BUSY`.

**Proposed**:
- Use `sqlite3_busy_handler()` or `sqlite3_busy_timeout()` instead of manual retry
- Set a reasonable timeout: `sqlite3_busy_timeout(_db, 5000);` (5 seconds)

**Pros**:
- SQLite's built-in busy handler is more efficient (uses exponential backoff internally)
- Applies to ALL operations automatically, not just `executeStep`
- Simpler code

**Cons**:
- Less control over retry behavior per-operation
- 5-second timeout might be too long or too short for some cases

**Migration**: Code-only. Replace manual retry with `sqlite3_busy_timeout()` call after open.

---

## Phase 7: Backup Friendliness (Low Risk, Low Impact)

### 7A. Online Backup API

**Current**: User must stop the application to safely copy `chain.db`.

**Proposed**: Expose SQLite's online backup API (`sqlite3_backup_init/step/finish`) through a new method:
```cpp
void Database::backup(const std::string& destinationPath);
```

Optionally trigger via RPC command.

**Pros**:
- Non-blocking backup while application is running
- Uses SQLite's official backup mechanism - guaranteed consistent
- Simple to implement (~20 lines of code)
- Preserves the "easy backup" philosophy (just copy one file)

**Cons**:
- Backup duration depends on database size
- Destination needs enough disk space
- Under WAL mode, should run `wal_checkpoint` first for smallest backup

**Migration**: Code-only. Add method + optional RPC endpoint.

### 7B. Pre-Backup WAL Checkpoint

If WAL mode is adopted (Phase 1A), provide a method to checkpoint before backup:
```cpp
void Database::prepareForBackup() {
    sqlite3_wal_checkpoint_v2(_db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
}
```

This merges the WAL file back into the main database, so the user only needs to copy `chain.db`.

**Pros**:
- Single-file backup (matches current user expectation)
- Clean state for backup

**Cons**:
- Briefly blocks writers during truncate checkpoint
- Only needed if WAL mode is adopted

---

## Recommended Implementation Order

| Priority | Phase | Effort | Risk | Impact | Needs Migration |
|----------|-------|--------|------|--------|-----------------|
| 1 | 1A - WAL mode | Low | Low | High | No (PRAGMA only) |
| 2 | 1B - synchronous=NORMAL | Low | Low | High | No |
| 3 | 1C - Cache size increase | Low | Low | Medium | No |
| 4 | 1D - mmap | Low | Low | Medium | No |
| 5 | 6B - Fix transaction error handling | Low | Low | High (stability) | No |
| 6 | 6C - sqlite3_busy_timeout | Low | Low | Medium (stability) | No |
| 7 | 2A - Covering indexes | Low | Low | High | No |
| 8 | 5B - Batched pruning | Low | Low | Medium | No |
| 9 | 6A - Integrity check on startup | Low | Low | Medium (stability) | No |
| 10 | 4A - Fix N+1 queries | Medium | Low | Medium | No |
| 11 | 3A - Read connection pool | Medium | Medium | High | No |
| 12 | 7A - Online backup API | Low | Low | Low | No |
| 13 | 2C - UTXO table split | High | Medium | Very High | Yes (v7) |
| 14 | 5C - Integer PK for utxos | High | Medium | High | Yes (v7) |
| 15 | 5A - Auto-vacuum | Medium | Medium | Low | Yes (rebuild) |
| 16 | 2B - WITHOUT ROWID | Medium | Low | Low | Yes (v7) |

**Items 1-9 require no schema migration and can be deployed incrementally with near-zero risk.**

Items 13-14 (UTXO restructuring) provide the largest performance gains but require a v7 migration converter. These could be combined into a single migration pass.

---

## Version 7 Migration Converter Design

If Phase 2C and/or 5C are adopted, the v6 -> v7 migration would:

```
1. BEGIN TRANSACTION
2. CREATE TABLE utxos_live (...new schema...)
3. CREATE TABLE utxos_spent (...new schema...)
4. INSERT INTO utxos_live SELECT ... FROM utxos WHERE heightDestroyed IS NULL
5. INSERT INTO utxos_spent SELECT ... FROM utxos WHERE heightDestroyed IS NOT NULL
6. DROP TABLE utxos
7. CREATE indexes on both new tables
8. UPDATE flags SET value=7 WHERE key='dbVersion'
9. COMMIT
```

This follows the existing migration pattern in `buildTables()` - add a new lambda at index 6 for the v6->v7 transition. The migration is atomic (wrapped in a transaction) so it either fully succeeds or fully rolls back.

**Estimated migration time**: Depends on UTXO count. For a mature DigiByte chain with millions of UTXOs, expect 5-30 minutes. Should display progress to user.

---

## Cross-Platform Notes

All proposed changes use standard SQLite features available on macOS, Linux, and Windows:
- WAL mode: supported on all platforms with local filesystems
- mmap: supported on all 64-bit platforms (skip or reduce on 32-bit)
- PRAGMAs: all proposed PRAGMAs are cross-platform
- `sqlite3_backup_*` API: cross-platform
- No OS-specific code required

The only platform consideration is `PRAGMA mmap_size` which should be conditional:
```cpp
#if INTPTR_MAX == INT64_MAX
    // 64-bit: use 256MB mmap
    sqlite3_exec(_db, "PRAGMA mmap_size = 268435456;", ...);
#else
    // 32-bit: use 32MB or skip
    sqlite3_exec(_db, "PRAGMA mmap_size = 33554432;", ...);
#endif
```
