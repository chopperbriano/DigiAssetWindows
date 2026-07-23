//
// Created by mctrivia on 04/10/23.
//
// Log.h - declares the Log singleton, the node/pool process's central logger.
//
// A single shared Log instance routes messages to the console (or to the
// ConsoleDashboard when one is attached) and, optionally, appends them to a log
// file. Each message carries a severity LogLevel; independent thresholds control
// what reaches the screen versus the file. All logging is mutex-guarded so it is
// safe to call from the node's many worker threads.
//

#ifndef DIGIASSET_CORE_LOG_H
#define DIGIASSET_CORE_LOG_H



#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

using namespace std;

// Windows headers define an ERROR macro; undefine it so our ERROR enumerator compiles.
#ifdef ERROR
#undef ERROR
#endif

class ConsoleDashboard; // forward declaration

class Log {
public:
    // Message severities in ascending order; numeric gaps leave room for future levels.
    enum LogLevel {
        DEBUG = 0,
        INFO = 10,
        WARNING = 20,
        ERROR = 30,
        CRITICAL = 40
    };

    /**
 * Singleton Start
 */
private:
    static Log* _pinstance;
    static std::mutex& getLock(); //heap allocated, never destroyed: background threads
                                   //may still log during process exit(static destruction)

protected:
    Log() = default;
    ~Log();

public:
    Log(Log& other) = delete;
    void operator=(const Log&) = delete;
    // Returns the singleton, creating it on first call.
    static Log* GetInstance();
    // Returns the singleton (creating it if needed) and opens the given log file on it.
    static Log* GetInstance(const std::string& fileName);

    /**
 * Singleton End
 */
private:
    ofstream _logFile;
    LogLevel _minLevelToScreen = INFO;
    LogLevel _minLevelToFile = INFO;
    ConsoleDashboard* _dashboard = nullptr;

public:
    // Open (append mode) the file that messages at/above the file threshold are written to.
    void setLogFile(const string& filename);
    void setMinLevelToScreen(LogLevel level);
    void setMinLevelToFile(LogLevel level);
    // Attach a dashboard; while set, screen output is routed to it instead of std::cout.
    void setDashboard(ConsoleDashboard* dashboard);
    // Format and emit a message to screen and/or file according to the level thresholds.
    void addMessage(const string& message, LogLevel level = INFO);


    /*
    ███████╗██████╗ ██████╗  ██████╗ ██████╗ ███████╗
    ██╔════╝██╔══██╗██╔══██╗██╔═══██╗██╔══██╗██╔════╝
    █████╗  ██████╔╝██████╔╝██║   ██║██████╔╝███████╗
    ██╔══╝  ██╔══██╗██╔══██╗██║   ██║██╔══██╗╚════██║
    ███████╗██║  ██║██║  ██║╚██████╔╝██║  ██║███████║
    ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝
     */

    class exception : public std::exception {
    protected:
        std::string _lastErrorMessage;
        mutable std::string _fullErrorMessage;

    public:
        explicit exception(const std::string& message = "Unknown") : _lastErrorMessage(message) {}

        virtual const char* what() const noexcept override {
            _fullErrorMessage = "Log Exception: " + _lastErrorMessage;
            return _fullErrorMessage.c_str();
        }
    };

    class exceptionFailedToOpen : public exception {
    public:
        explicit exceptionFailedToOpen()
            : exception("Couldn't open or create the log") {}
    };
};


#endif //DIGIASSET_CORE_LOG_H
