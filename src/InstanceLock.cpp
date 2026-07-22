//
// Created by mctrivia on 21/12/24.
//
// Cross-platform single-instance guard - see InstanceLock.h.
//

#include "InstanceLock.h"
#include <iostream>
#include <csignal>
#include <fstream>
#include <string>

int InstanceLock::_lockFileDescriptor = -1;

#ifdef _WIN32
// ============================== Windows ==============================
#include <windows.h>

// Constructor: build the named-mutex name. "Local\\" scopes it to the current
// login session; use "Global\\" to guard across all sessions on the machine.
InstanceLock::InstanceLock(const std::string &lockName) : _mutexHandle(nullptr) {
    _lockFilePath = "Local\\DigiAsset_" + lockName;
}

// Attempt to acquire the lock via a named mutex.
bool InstanceLock::acquire() {
    HANDLE h = CreateMutexA(nullptr, TRUE, _lockFilePath.c_str());
    if (h == nullptr) {
        std::cerr << "Failed to create instance lock (error " << GetLastError() << ")." << std::endl;
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "Another instance is already running." << std::endl;
        CloseHandle(h);
        return false;
    }
    // Held for the lifetime of the process; Windows releases it automatically on
    // normal exit OR crash, so no signal-handler cleanup is required.
    _mutexHandle = h;
    return true;
}

// Release the mutex (also happens automatically on process exit).
void InstanceLock::release() {
    if (_mutexHandle != nullptr) {
        ReleaseMutex((HANDLE) _mutexHandle);
        CloseHandle((HANDLE) _mutexHandle);
        _mutexHandle = nullptr;
    }
}

InstanceLock::~InstanceLock() {
    release();
}

#elif defined(__APPLE__)
// ============================== macOS ==============================

#include <mach-o/dyld.h>
#include <unistd.h>
#include <csignal>
#include <sys/file.h>
#include <fcntl.h>
#include <limits.h>

static std::string getExecutablePath() {
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string exePath(path);
        return exePath.substr(0, exePath.find_last_of('/'));
    }
    return ".";
}

InstanceLock::InstanceLock(const std::string& lockName) {
    _lockFilePath = getExecutablePath() + "/" + lockName + ".lock";
}

bool InstanceLock::acquire() {
    _lockFileDescriptor = open(_lockFilePath.c_str(), O_CREAT | O_RDWR, 0666);
    if (_lockFileDescriptor < 0) {
        std::cerr << "Failed to open or create lock file: " << _lockFilePath << std::endl;
        return false;
    }

    pid_t existingPid = 0;
    std::ifstream inFile(_lockFilePath);
    inFile >> existingPid;
    inFile.close();

    if (existingPid > 0 && kill(existingPid, 0) == 0) {
        std::cerr << "Another instance is already running (PID " << existingPid << ")." << std::endl;
        return false;
    }

    if (flock(_lockFileDescriptor, LOCK_EX | LOCK_NB) == -1) {
        std::cerr << "Another instance is already running (active lock)." << std::endl;
        return false;
    }

    ftruncate(_lockFileDescriptor, 0);
    dprintf(_lockFileDescriptor, "%d\n", getpid());

    setupSignalHandlers();
    return true;
}

void InstanceLock::setupSignalHandlers() {
    signal(SIGINT,  cleanupOnSignal);
    signal(SIGTERM, cleanupOnSignal);
    signal(SIGSEGV, cleanupOnSignal);
    signal(SIGABRT, cleanupOnSignal);
}

void InstanceLock::cleanupOnSignal(int sig) {
    if (_lockFileDescriptor != -1) {
        unlink(getExecutablePath().append("/digiasset_core.lock").c_str());
    }
    exit(sig);
}

InstanceLock::~InstanceLock() { release(); }

void InstanceLock::release() {
    if (_lockFileDescriptor != -1) {
        unlink(_lockFilePath.c_str());
        close(_lockFileDescriptor);
    }
}

#else // Linux

#include <unistd.h>
#include <csignal>
#include <sys/file.h>
#include <fcntl.h>
#include <limits.h>

static std::string getExecutablePath() {
    char path[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
    if (count != -1) {
        std::string exePath(path, count);
        return exePath.substr(0, exePath.find_last_of('/'));
    }
    return ".";
}

InstanceLock::InstanceLock(const std::string& lockName) {
    _lockFilePath = getExecutablePath() + "/" + lockName + ".lock";
}

bool InstanceLock::acquire() {
    _lockFileDescriptor = open(_lockFilePath.c_str(), O_CREAT | O_RDWR, 0666);
    if (_lockFileDescriptor < 0) {
        std::cerr << "Failed to open or create lock file: " << _lockFilePath << std::endl;
        return false;
    }

    pid_t existingPid = 0;
    std::ifstream inFile(_lockFilePath);
    inFile >> existingPid;
    inFile.close();

    if (existingPid > 0 && kill(existingPid, 0) == 0) {
        std::cerr << "Another instance is already running (PID " << existingPid << ")." << std::endl;
        return false;
    }

    if (flock(_lockFileDescriptor, LOCK_EX | LOCK_NB) == -1) {
        std::cerr << "Another instance is already running (active lock)." << std::endl;
        return false;
    }

    ftruncate(_lockFileDescriptor, 0);
    dprintf(_lockFileDescriptor, "%d\n", getpid());

    setupSignalHandlers();
    return true;
}

void InstanceLock::setupSignalHandlers() {
    signal(SIGINT,  cleanupOnSignal);
    signal(SIGTERM, cleanupOnSignal);
    signal(SIGSEGV, cleanupOnSignal);
    signal(SIGABRT, cleanupOnSignal);
}

void InstanceLock::cleanupOnSignal(int sig) {
    if (_lockFileDescriptor != -1) {
        unlink(getExecutablePath().append("/digiasset_core.lock").c_str());
    }
    exit(sig);
}

InstanceLock::~InstanceLock() { release(); }

void InstanceLock::release() {
    if (_lockFileDescriptor != -1) {
        unlink(_lockFilePath.c_str());
        close(_lockFileDescriptor);
    }
}

#endif
