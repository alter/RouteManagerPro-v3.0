// src/common/Logger.h
#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <filesystem>

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void Log(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!logFile_.is_open()) {
            // Создаем папку logs если её нет
            std::filesystem::create_directories("logs");
            logFile_.open("logs/route_manager.log", std::ios::app);
        }

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        char timeStr[100];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&time_t));

        logFile_ << "[" << timeStr << "] " << message << std::endl;
        logFile_.flush();
    }

    void Error(const std::string& message) {
        Log("ERROR: " + message);
    }

    void Info(const std::string& message) {
        Log("INFO: " + message);
    }

    void Debug(const std::string& message) {
        Log("DEBUG: " + message);
    }

    void Warning(const std::string& message) {
        Log("WARNING: " + message);
    }

private:
    Logger() {}
    ~Logger() {
        if (logFile_.is_open()) {
            logFile_.close();
        }
    }

    std::ofstream logFile_;
    std::mutex mutex_;
};