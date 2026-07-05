//
// Created by mctrivia on 20/03/24.
//

// Database_Statement.h - Declares the Statement class, a thread-safe wrapper around a single
// prepared SQLite statement (sqlite3_stmt). The Database layer of both the node and the pool
// server keeps a pool of these; each Statement owns one compiled query, guards concurrent use
// with a mutex, and records how long callers hold its lock for performance profiling. Callers
// never touch the raw sqlite3_stmt directly - they borrow it through a LockedStatement (declared
// a friend below), which acquires the mutex for the duration of one bind/step/reset cycle.

#ifndef DIGIASSET_CORE_DATABASE_STATEMENT_H
#define DIGIASSET_CORE_DATABASE_STATEMENT_H



#include "Database_LockedStatement.h"
#include <chrono>
#include <mutex>
#include <sqlite3.h>
#include <string>

// Owns one prepared SQLite statement plus the mutex that serializes access to it and running
// totals of how long/how often it has been locked. LockedStatement is a friend so it can reach
// the private _stmt and _mutex while a query is in flight.
class Statement {
public:
    Statement()=default;

    // Finalizes the owned sqlite3_stmt if one was prepared.
    ~Statement();

    // Compiles query against db and stores the resulting sqlite3_stmt. Must be called exactly once
    // before use; throws if this Statement was already prepared, or Database::exceptionCreating
    // Statement if SQLite rejects the SQL.
    void prepare(sqlite3* db, const std::string& query);

    // Accumulates one lock-hold sample: adds duration to the running total and increments the count
    // (used to profile which queries are contended). Called by LockedStatement when it releases.
    void addLockDuration(const std::chrono::microseconds& duration);

    long long getTotalLockDuration() const;
    int getLockCount() const;

    friend class LockedStatement;
private:
    sqlite3_stmt* _stmt = nullptr;
    std::mutex _mutex;
    long long _totalLockedDuration = 0;
    int _lockCount = 0;
};



#endif //DIGIASSET_CORE_DATABASE_STATEMENT_H
