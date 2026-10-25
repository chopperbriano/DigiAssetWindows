// PoolLog.cpp - see PoolLog.h.

#include "PoolLog.h"
#include "Log.h"
#include <mutex>

namespace {
    std::mutex& pathLock() {
        static std::mutex* m = new std::mutex; //leaked: threads may still log during exit
        return *m;
    }
    std::string& currentPath() {
        static std::string* p = new std::string("pool.log");
        return *p;
    }
}

void PoolLog::setPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(pathLock());
    currentPath() = path;
}

void PoolLog::line(const std::string& text) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(pathLock());
        path = currentPath();
    }
    if (path.empty()) return;
    std::string clean = text; //one record per line, whatever the message contained
    for (char& c: clean) if ((c == '\r') || (c == '\n')) c = ' ';
    try {
        Log::appendToSideFile(path, clean, 50ull * 1024 * 1024);
    } catch (...) {}
}
