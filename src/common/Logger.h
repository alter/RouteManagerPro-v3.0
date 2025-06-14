// src/common/Logger.h
#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <format>
#include <deque>
#include <thread>
#include <condition_variable>
#include <atomic>

#ifdef __cpp_lib_stacktrace
#include <stacktrace>
#endif

class Logger {
public:
    enum class LogLevel {
        LEVEL_DEBUG = 0,
        LEVEL_INFO = 1,
        LEVEL_WARNING = 2,
        LEVEL_ERROR = 3
    };

    struct LogConfig {
        size_t maxFileSize = 10 * 1024 * 1024;  // 10MB
        size_t maxFiles = 5;
        bool asyncLogging = true;
        size_t bufferSize = 1000;
        std::chrono::milliseconds flushInterval{ 1000 };
    };

    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLogLevel(LogLevel level) {
        currentLogLevel = level;
    }

    void SetConfig(const LogConfig& cfg) {
        std::lock_guard<std::mutex> lock(configMutex);
        config = cfg;

        if (config.asyncLogging && !asyncThread.joinable()) {
            StartAsyncLogging();
        }
        else if (!config.asyncLogging && asyncThread.joinable()) {
            StopAsyncLogging();
        }
    }

    void Log(const std::string& message) {
        Log(message, LogLevel::LEVEL_INFO);
    }

    void Log(const std::string& message, LogLevel level) {
        if (level < currentLogLevel) {
            return;
        }

        auto now = std::chrono::system_clock::now();
        LogEntry entry{ now, level, message };

        if (config.asyncLogging) {
            std::lock_guard<std::mutex> lock(bufferMutex);
            if (buffer.size() >= config.bufferSize) {
                buffer.pop_front();
            }
            buffer.push_back(entry);
            bufferCV.notify_one();
        }
        else {
            WriteEntry(entry);
        }
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

    void Flush() {
        if (config.asyncLogging) {
            std::unique_lock<std::mutex> lock(bufferMutex);
            while (!buffer.empty()) {
                bufferCV.wait(lock);
            }
        }

        std::lock_guard<std::mutex> lock(fileMutex);
        if (logFile_.is_open()) {
            logFile_.flush();
        }
    }

#ifdef __cpp_lib_stacktrace
    void LogWithStackTrace(const std::string& message, LogLevel level) {
        Log(message, level);
        if (level >= LogLevel::LEVEL_ERROR) {
            auto trace = std::stacktrace::current();
            for (const auto& entry : trace) {
                Log(std::format("  at {}", entry.description()), LogLevel::LEVEL_DEBUG);
            }
        }
    }
#endif

    ~Logger() {
        if (config.asyncLogging) {
            StopAsyncLogging();
        }

        if (logFile_.is_open()) {
            logFile_.close();
        }
    }

private:
    Logger() {
#ifdef _DEBUG
        currentLogLevel = LogLevel::LEVEL_DEBUG;
#else
        currentLogLevel = LogLevel::LEVEL_INFO;
#endif
        if (config.asyncLogging) {
            StartAsyncLogging();
        }
    }

    struct LogEntry {
        std::chrono::system_clock::time_point timestamp;
        LogLevel level;
        std::string message;
    };

    void StartAsyncLogging() {
        asyncRunning = true;
        asyncThread = std::thread(&Logger::AsyncLogThread, this);
    }

    void StopAsyncLogging() {
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            asyncRunning = false;
            bufferCV.notify_all();
        }

        if (asyncThread.joinable()) {
            asyncThread.join();
        }
    }

    void AsyncLogThread() {
        while (asyncRunning) {
            std::unique_lock<std::mutex> lock(bufferMutex);

            bufferCV.wait_for(lock, config.flushInterval, [this] {
                return !buffer.empty() || !asyncRunning;
                });

            if (!buffer.empty()) {
                std::deque<LogEntry> localBuffer;
                localBuffer.swap(buffer);
                lock.unlock();

                for (const auto& entry : localBuffer) {
                    WriteEntry(entry);
                }
            }
        }

        // Flush remaining entries
        for (const auto& entry : buffer) {
            WriteEntry(entry);
        }
    }

    void WriteEntry(const LogEntry& entry) {
        std::lock_guard<std::mutex> lock(fileMutex);

        CheckRotation();

        if (!logFile_.is_open()) {
            OpenLogFile();
        }

        auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
        char timeStr[100];
        struct tm timeinfo;
        localtime_s(&timeinfo, &time_t);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

        logFile_ << "[" << timeStr << "] "
            << GetLevelString(entry.level) << " "
            << entry.message << std::endl;
    }

    void OpenLogFile() {
        std::filesystem::create_directories("logs");
        currentLogPath = "logs/route_manager.log";
        logFile_.open(currentLogPath, std::ios::app);
        currentFileSize = std::filesystem::exists(currentLogPath) ?
            std::filesystem::file_size(currentLogPath) : 0;
    }

    void CheckRotation() {
        if (!logFile_.is_open() || currentFileSize < config.maxFileSize) {
            return;
        }

        logFile_.close();

        // Rotate files
        for (size_t i = config.maxFiles - 1; i > 0; --i) {
            auto oldPath = std::format("logs/route_manager.{}.log", i - 1);
            auto newPath = std::format("logs/route_manager.{}.log", i);

            if (std::filesystem::exists(oldPath)) {
                std::filesystem::rename(oldPath, newPath);
            }
        }

        std::filesystem::rename(currentLogPath, "logs/route_manager.0.log");

        OpenLogFile();
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

    // Configuration
    LogConfig config;
    std::mutex configMutex;

    // File handling
    std::ofstream logFile_;
    std::string currentLogPath;
    size_t currentFileSize = 0;
    std::mutex fileMutex;

    // Log level
    LogLevel currentLogLevel;

    // Async logging
    std::deque<LogEntry> buffer;
    std::mutex bufferMutex;
    std::condition_variable bufferCV;
    std::thread asyncThread;
    std::atomic<bool> asyncRunning{ false };
};