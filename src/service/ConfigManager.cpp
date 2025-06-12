// src/service/ConfigManager.cpp
#include "ConfigManager.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <fstream>
#include <json/json.h>
#include <chrono>
#include <thread>
#include <filesystem>

ConfigManager::ConfigManager() : configDirty(false),
lastSaveTime(std::chrono::steady_clock::now()),
persistThread(&ConfigManager::PersistenceThreadFunc, this) {
    configPath = Utils::GetCurrentDirectory() + "\\" + Constants::CONFIG_FILE;
    LoadConfig();
}

ConfigManager::~ConfigManager() {
    running = false;
    if (persistThread.joinable()) {
        persistThread.join();
    }

    // Final save on shutdown
    if (configDirty.load()) {
        Logger::Instance().Info("ConfigManager shutdown: Saving config to disk");
        SaveConfig();
    }
}

void ConfigManager::PersistenceThreadFunc() {
    Logger::Instance().Info("ConfigManager persistence thread started");

    try {
        while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
            // Check every minute
            for (int i = 0; i < 60 && running.load() && !ShutdownCoordinator::Instance().isShuttingDown; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }

            auto now = std::chrono::steady_clock::now();
            auto timeSinceLastSave = now - lastSaveTime;

            // Save if dirty and 10 minutes have passed
            if (configDirty.load() && timeSinceLastSave >= SAVE_INTERVAL) {
                Logger::Instance().Info("Periodic save of config (dirty flag set)");
                SaveConfig();
            }
        }

        // Final save on thread exit
        if (configDirty.load()) {
            Logger::Instance().Info("ConfigManager persistence thread: Final save of config");
            SaveConfig();
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("ConfigManager::PersistenceThreadFunc exception: " + std::string(e.what()));
    }

    // Log AFTER all work is done, right before thread exits
    Logger::Instance().Info("ConfigManager persistence thread exiting");
}

ServiceConfig ConfigManager::GetConfig() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return config;
}

void ConfigManager::SetConfig(const ServiceConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;
    configDirty = true;  // Mark as dirty instead of saving immediately

    Logger::Instance().Info("ConfigManager::SetConfig - Updated " +
        std::to_string(config.selectedProcesses.size()) + " selected processes (marked dirty)");
    for (const auto& proc : config.selectedProcesses) {
        Logger::Instance().Debug("  - " + proc);
    }
}

void ConfigManager::SetAIPreloadEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(configMutex);
    config.aiPreloadEnabled = enabled;
    configDirty = true;  // Mark as dirty instead of saving immediately
}

void ConfigManager::LoadConfig() {
    if (!Utils::FileExists(configPath)) {
        Logger::Instance().Info("ConfigManager::LoadConfig - Config file not found, using defaults");
        config = GetDefaultConfig();
        SaveConfig();  // Save defaults immediately
        return;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        Logger::Instance().Error("ConfigManager::LoadConfig - Failed to open file: " + configPath);
        config = GetDefaultConfig();
        return;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;

    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        Logger::Instance().Error("ConfigManager::LoadConfig - Parse error: " + errors);
        config = GetDefaultConfig();
        return;
    }

    config.gatewayIp = root.get("gatewayIp", config.gatewayIp).asString();
    config.metric = root.get("metric", config.metric).asInt();
    config.startMinimized = root.get("startMinimized", config.startMinimized).asBool();
    config.startWithWindows = root.get("startWithWindows", config.startWithWindows).asBool();
    config.aiPreloadEnabled = root.get("aiPreloadEnabled", config.aiPreloadEnabled).asBool();

    const Json::Value& processes = root["selectedProcesses"];
    if (processes.isArray()) {
        config.selectedProcesses.clear();
        for (const auto& process : processes) {
            config.selectedProcesses.push_back(process.asString());
        }

        Logger::Instance().Info("ConfigManager::LoadConfig - Loaded " +
            std::to_string(config.selectedProcesses.size()) + " selected processes");
        for (const auto& proc : config.selectedProcesses) {
            Logger::Instance().Debug("  - " + proc);
        }
    }

    file.close();
    configDirty = false;  // Reset dirty flag after loading
}

void ConfigManager::SaveConfig() {
    Logger::Instance().Info("ConfigManager::SaveConfig - Saving configuration with " +
        std::to_string(config.selectedProcesses.size()) + " selected processes");

    // Create a copy of config under lock
    ServiceConfig configCopy;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        configCopy = config;
    }

    // Save without holding the lock
    Json::Value root;
    root["gatewayIp"] = configCopy.gatewayIp;
    root["metric"] = configCopy.metric;
    root["startMinimized"] = configCopy.startMinimized;
    root["startWithWindows"] = configCopy.startWithWindows;
    root["aiPreloadEnabled"] = configCopy.aiPreloadEnabled;

    Json::Value processes(Json::arrayValue);
    for (const auto& process : configCopy.selectedProcesses) {
        processes.append(process);
        Logger::Instance().Debug("  Saving process: " + process);
    }
    root["selectedProcesses"] = processes;

    std::ofstream file(configPath + ".tmp");
    if (!file.is_open()) {
        Logger::Instance().Error("ConfigManager::SaveConfig - Failed to open file: " + configPath + ".tmp");
        return;
    }

    Json::StreamWriterBuilder builder;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(root, &file);
    file.close();

    // Atomic rename
    if (std::filesystem::exists(configPath + ".tmp")) {
        std::filesystem::rename(configPath + ".tmp", configPath);
    }

    configDirty = false;
    lastSaveTime = std::chrono::steady_clock::now();

    Logger::Instance().Debug("ConfigManager::SaveConfig - Saved to " + configPath);
}

ServiceConfig ConfigManager::GetDefaultConfig() {
    ServiceConfig defaultConfig;
    defaultConfig.selectedProcesses = {
        "Discord.exe"
    };
    return defaultConfig;
}