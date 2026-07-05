//
// Created by mctrivia on 20/03/24.
//

// Database_LockedStatement.cpp
// Implementation of LockedStatement - the RAII lock/reset/bind/step/read wrapper
// over a Statement's reusable sqlite3_stmt. See Database_LockedStatement.h for
// the role this plays in serializing access to shared prepared statements.

#include "Database_Statement.h"
#include "Database_LockedStatement.h"

// Locks the owning Statement's mutex (via the _lock member initializer, giving
// this thread exclusive use of the sqlite3_stmt), records the current time for
// later lock-duration profiling, caches the raw statement pointer, and resets
// the statement so it is ready to be re-bound and re-executed.
LockedStatement::LockedStatement(Statement& statement)
    : _lock(statement._mutex), _creator(&statement), _creationTime(std::chrono::steady_clock::now()) {
    _stmt = statement._stmt;
    // The lock is acquired as soon as an object of this class is created
    reset();
}

// Reports how long the lock was held to the owning Statement (for profiling)
// and, as the _lock member is destroyed, releases the mutex.
LockedStatement::~LockedStatement() {
    auto duration = std::chrono::steady_clock::now() - _creationTime;
    _creator->addLockDuration(std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

void LockedStatement::reset() {
    sqlite3_reset(_stmt);
}

// Bind methods remember indexes start at 1
void LockedStatement::bindInt(int index, int value) {
    sqlite3_bind_int(_stmt, index, value);
}

void LockedStatement::bindInt64(int index, int64_t value) {
    sqlite3_bind_int64(_stmt, index, value);
}

void LockedStatement::bindDouble(int index, double value) {
    sqlite3_bind_double(_stmt, index, value);
}

// Binds a text parameter. SQLITE_TRANSIENT makes sqlite copy the string, so the
// bound value stays valid even after the source string is destroyed.
void LockedStatement::bindText(int index, const std::string& value) {
    sqlite3_bind_text(_stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

// Binds a blob parameter. SQLITE_TRANSIENT makes sqlite copy the bytes, so the
// bound value stays valid even after the source Blob is destroyed.
void LockedStatement::bindBlob(int index, const Blob& blob) {
    sqlite3_bind_blob(_stmt, index, blob.data(), blob.length(), SQLITE_TRANSIENT);
}

void LockedStatement::bindNull(int index) {
    sqlite3_bind_null(_stmt, index);
}

// Get column methods remember indexes start at 0
int LockedStatement::getColumnInt(int index) {
    return sqlite3_column_int(_stmt, index);
}

int64_t LockedStatement::getColumnInt64(int index) {
    return sqlite3_column_int64(_stmt, index);
}

double LockedStatement::getColumnDouble(int index) {
    return sqlite3_column_double(_stmt, index);
}

// Reads a text column as a std::string. Warning: does not guard against a NULL
// column value (would construct a string from a null pointer); callers rely on
// the query/schema guaranteeing the column is non-NULL.
std::string LockedStatement::getColumnText(int index) {
    const unsigned char* text = sqlite3_column_text(_stmt, index);
    return std::string(reinterpret_cast<const char*>(text));
}

// Reads a blob column, copying its bytes into a Blob of the reported length.
Blob LockedStatement::getColumnBlob(int index) {
    const void* data = sqlite3_column_blob(_stmt, index);
    int length = sqlite3_column_bytes(_stmt, index);
    return Blob(data, length);
}

// Execute step
int LockedStatement::executeStep() {
    return sqlite3_step(_stmt);
}