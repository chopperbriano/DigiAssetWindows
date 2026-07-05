//
// Created by mctrivia on 04/10/23.
//
// Log.cpp - implementation of the Log singleton (see Log.h). Provides thread-safe
// creation of the shared logger and dispatches formatted, severity-tagged messages
// to the console/dashboard and to the optional append-mode log file.
//

#include "Log.h"
#include "ConsoleDashboard.h"

std::mutex Log::_mutex;
Log* Log::_pinstance = nullptr;

// Return the singleton instance, lazily constructing it under lock on first use.
Log* Log::GetInstance() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pinstance == nullptr) {
        _pinstance = new Log();
    }
    return _pinstance;
}

// Return the singleton (constructing it if needed) and open fileName as its log file.
Log* Log::GetInstance(const string& fileName) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pinstance == nullptr) {
        _pinstance = new Log();
    }
    _pinstance->setLogFile(fileName);
    return _pinstance;
}

// Open the log file in append mode; throws exceptionFailedToOpen if it cannot be opened.
void Log::setLogFile(const string& filename) {
    _logFile.open(filename, ios_base::app);
    if (!_logFile.is_open()) {
        throw exceptionFailedToOpen();
    }
}

void Log::setMinLevelToScreen(LogLevel level) {
    _minLevelToScreen = level;
}

void Log::setMinLevelToFile(LogLevel level) {
    _minLevelToFile = level;
}

void Log::setDashboard(ConsoleDashboard* dashboard) {
    _dashboard = dashboard;
}

// Prefix the message with its level name, then (under lock) print it to the dashboard
// or std::cout if it meets the screen threshold, and append it to the file if open and
// it meets the file threshold.
void Log::addMessage(const string& message, LogLevel level) {
    lock_guard<mutex> lock(_mutex);

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
        if (_dashboard) {
            _dashboard->addMessage(logMessage);
        } else {
            cout << logMessage << endl;
        }
    }

    if (level >= _minLevelToFile && _logFile.is_open()) {
        _logFile << logMessage << endl;
    }
}

// Close the log file on destruction if it is still open.
Log::~Log() {
    if (_logFile.is_open()) {
        _logFile.close();
    }
}