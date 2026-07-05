//
// Created by mctrivia on 20/03/24.
//

// Database_Statement.cpp - Implements the Statement wrapper declared in Database_Statement.h.
// Handles preparing (compiling) a SQLite query, finalizing it on destruction, and tracking the
// lock-duration statistics that the Database layer uses to profile query contention.

#include "Database_Statement.h"
#include "AppMain.h"
#include "Database.h"
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <thread>



// Releases the compiled statement back to SQLite so its resources are freed.
Statement::~Statement() {
    if (_stmt!= nullptr) sqlite3_finalize(_stmt);
}

// Compiles query into _stmt via sqlite3_prepare_v2. Guards against double-prepare (a logic error)
// and, on any SQLite failure, prints the DB error message and throws exceptionCreatingStatement.
void Statement::prepare(sqlite3* db, const std::string& query) {
    if (_stmt!= nullptr) throw std::runtime_error("Statement already prepared");    //code is wrong if this executes
    const char* tail;
    int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &_stmt, &tail);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot prepare statement: %s\n", sqlite3_errmsg(db));
        throw Database::exceptionCreatingStatement();
    }
}

// Records one lock-hold sample: adds the microsecond count to the running total and bumps the
// lock count. Not internally synchronized - the caller holds the statement lock when calling this.
void Statement::addLockDuration(const std::chrono::microseconds& duration) {
    _totalLockedDuration += duration.count();
    _lockCount++;
}

long long Statement::getTotalLockDuration() const { return _totalLockedDuration; }
int Statement::getLockCount() const { return _lockCount; }
