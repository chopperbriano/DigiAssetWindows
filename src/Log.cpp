//
// Created by mctrivia on 04/10/23.
//
// Log.cpp - implementation of the Log singleton (see Log.h). Provides thread-safe
// creation of the shared logger and dispatches formatted, severity-tagged messages
// to the console/dashboard and to the optional append-mode log file.
//

#include "Log.h"
// ConsoleDashboard.h deliberately NOT included - see _dashboardSink in Log.h
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

std::mutex& Log::getLock() {
    static std::mutex* mutex = new std::mutex; //intentionally leaked - see header
    return *mutex;
}
Log* Log::_pinstance = nullptr;

// Current local time as "YYYY-MM-DD HH:MM:SS" for log-file / console lines.
static std::string logStamp() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// Return the singleton instance, lazily constructing it under lock on first use.
Log* Log::GetInstance() {
    std::lock_guard<std::mutex> lock(getLock());
    if (_pinstance == nullptr) {
        _pinstance = new Log();
    }
    return _pinstance;
}

// Return the singleton (constructing it if needed) and open fileName as its log file.
Log* Log::GetInstance(const string& fileName) {
    std::lock_guard<std::mutex> lock(getLock());
    if (_pinstance == nullptr) {
        _pinstance = new Log();
    }
    _pinstance->setLogFile(fileName);
    return _pinstance;
}

// Open the log file in append mode; throws exceptionFailedToOpen if it cannot be opened.
void Log::setLogFile(const string& filename) {
    if (_logFile.is_open()) _logFile.close();
    _logFileName = filename;
    _logFile.open(filename, ios_base::app);
    if (!_logFile.is_open()) {
        throw exceptionFailedToOpen();
    }
}

void Log::setMaxFileBytes(uint64_t bytes) {
    _maxFileBytes = bytes;
}

// Moves a full file to <name>.1 (replacing any older one).  Called with the lock held.
static void rotateFile(const string& filename) {
    string old = filename + ".1";
    std::remove(old.c_str());
    std::rename(filename.c_str(), old.c_str());
}

// Called with the lock held, after a write.  tellp on an append stream is the file size.
void Log::_rotateIfFull() {
    if ((_maxFileBytes == 0) || _logFileName.empty()) return;
    std::streamoff size = _logFile.tellp();
    if ((size < 0) || (static_cast<uint64_t>(size) < _maxFileBytes)) return;
    _logFile.close();
    rotateFile(_logFileName);
    _logFile.open(_logFileName, ios_base::app);
}

//one lock for every side file, shared by appendToSideFile and clearSideFile
static std::mutex& sideLock() {
    static std::mutex* m = new std::mutex; //leaked like getLock(): may be used during exit
    return *m;
}

void Log::appendToSideFile(const string& filename, const string& line, uint64_t maxBytes) {
    lock_guard<mutex> lock(sideLock());
    {
        ofstream out(filename, ios_base::app);
        if (!out.is_open()) return; //a record we cannot keep must never stop the node
        out << logStamp() << "\t" << line << "\n";
        std::streamoff size = out.tellp();
        if ((maxBytes == 0) || (size < 0) || (static_cast<uint64_t>(size) < maxBytes)) return;
    }
    rotateFile(filename);
}

// Size of a file and its rotated .1 copy; a missing file counts as 0.
static uint64_t sizeWithRotated(const string& filename) {
    uint64_t total = 0;
    for (const string& f: {filename, filename + ".1"}) {
        struct stat st;
        if (stat(f.c_str(), &st) == 0) total += static_cast<uint64_t>(st.st_size);
    }
    return total;
}

uint64_t Log::clearLogFile() {
    lock_guard<mutex> lock(getLock());
    if (_logFileName.empty()) return 0;
    uint64_t before = sizeWithRotated(_logFileName);
    _logFile.close();
    std::remove((_logFileName + ".1").c_str());
    _logFile.open(_logFileName, ios_base::out | ios_base::trunc);
    if (!_logFile.is_open()) _logFile.open(_logFileName, ios_base::app); //held open elsewhere: keep logging
    uint64_t after = sizeWithRotated(_logFileName);
    return (before > after) ? before - after : 0;
}

uint64_t Log::clearSideFile(const string& filename) {
    lock_guard<mutex> lock(sideLock());
    uint64_t before = sizeWithRotated(filename);
    struct stat st;
    if (stat(filename.c_str(), &st) == 0) { ofstream out(filename, ios_base::out | ios_base::trunc); }
    std::remove((filename + ".1").c_str());
    uint64_t after = sizeWithRotated(filename);
    return (before > after) ? before - after : 0;
}

void Log::setMinLevelToScreen(LogLevel level) {
    _minLevelToScreen = level;
}

void Log::setMinLevelToFile(LogLevel level) {
    _minLevelToFile = level;
}

void Log::setDashboardSink(std::function<void(const std::string&)> sink) {
    _dashboardSink = std::move(sink);
}

// Prefix the message with its level name, then (under lock) print it to the dashboard
// or std::cout if it meets the screen threshold, and append it to the file if open and
// it meets the file threshold.
void Log::addMessage(const string& message, LogLevel level) {
    lock_guard<mutex> lock(getLock());

    string logLevelStr;
    switch (level) {
        case DEBUG:
            logLevelStr = "DEBUG";
            break;
        case INFO:
            logLevelStr = "INFO";
            break;
        case WARNING:
            logLevelStr = "WARNING";
            break;
        case ERROR:
            logLevelStr = "ERROR";
            break;
        case CRITICAL:
            logLevelStr = "CRITICAL";
            break;
    }

    string logMessage = logLevelStr + ": " + message;

    if (level >= _minLevelToScreen) {
        if (_dashboardSink) {
            _dashboardSink(logMessage);   // the dashboard prepends its own screen timestamp
        } else {
            cout << logStamp() << " " << logMessage << endl;
        }
    }

    if (level >= _minLevelToFile && _logFile.is_open()) {
        _logFile << logStamp() << " " << logMessage << endl;
        _rotateIfFull();
    }
}

// Close the log file on destruction if it is still open.
Log::~Log() {
    if (_logFile.is_open()) {
        _logFile.close();
    }
}