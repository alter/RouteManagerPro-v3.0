// src/common/Logger.h
#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <ctime>

class Logger {
public:
    enum class LogLevel {
        LEVEL_DEBUG = 0,
        LEVEL_INFO = 1,
        LEVEL_WARNING = 2,
        LEVEL_ERROR = 3
    };

    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLogLevel(LogLevel level) {
        currentLogLevel = level;
    }

    void Log(const std::string& message) {
        Log(message, LogLevel::LEVEL_INFO);
    }

    void Log(const std::string& message, LogLevel level) {
        if (level < currentLogLevel) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!logFile_.is_open()) {
            std::filesystem::create_directories("logs");
            logFile_.open("logs/route_manager.log", std::ios::app);
        }

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        char timeStr[100];
        struct tm timeinfo;
        localtime_s(&timeinfo, &time_t);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

        logFile_ << "[" << timeStr << "] " << message << std::endl;
        logFile_.flush();
    }

    void Error(const std::string& message) {
        Log("ERROR: " + message, LogLevel::LEVEL_ERROR);
    }

    void Info(const std::string& message) {
        Log("INFO: " + message, LogLevel::LEVEL_INFO);
    }

    void Debug(const std::string& message) {
        Log("DEBUG: " + message, LogLevel::LEVEL_DEBUG);
    }

    void Warning(const std::string& message) {
        Log("WARNING: " + message, LogLevel::LEVEL_WARNING);
    }

private:
    Logger() {
#ifdef _DEBUG
        currentLogLevel = LogLevel::LEVEL_DEBUG;
#else
        currentLogLevel = LogLevel::LEVEL_INFO;
#endif
    }

    ~Logger() {
        if (logFile_.is_open()) {
            logFile_.close();
        }
    }

    std::string GetLevelString(LogLevel level) {
        switch (level) {
        case LogLevel::LEVEL_DEBUG: return "[DEBUG]";
        case LogLevel::LEVEL_INFO: return "[INFO]";
        case LogLevel::LEVEL_WARNING: return "[WARN]";
        case LogLevel::LEVEL_ERROR: return "[ERROR]";
        default: return "[UNKNOWN]";
        }
    }

    std::ofstream logFile_;
    std::mutex mutex_;
    LogLevel currentLogLevel;
};