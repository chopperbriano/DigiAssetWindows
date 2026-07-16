//
// Created by mctrivia on 21/12/24.
//
// Cross-platform single-instance guard.
//   - Windows: a named mutex (the OS releases it automatically on process exit
//     OR crash, so no signal-handler cleanup is needed - which also means it does
//     NOT install SIGINT/SIGTERM handlers that would fight the node's graceful
//     shutdown).
//   - POSIX:   a PID lock file + flock (the original mctrivia implementation).
//

#ifndef INSTANCELOCK_H
#define INSTANCELOCK_H

#include <string>

class InstanceLock {
public:
    explicit InstanceLock(const std::string &lockName);
    ~InstanceLock();

    // Attempts to acquire the lock, returns true if successful
    bool acquire();

    // Releases the lock (optional, auto-cleans on exit)
    void release();

private:
    std::string _lockFilePath;
#ifdef _WIN32
    void* _mutexHandle;            // HANDLE to the named mutex (nullptr = not held)
#else
    static int _lockFileDescriptor;
    void setupSignalHandlers();
    static void cleanupOnSignal(int sig);
#endif
};

#endif // INSTANCELOCK_H
