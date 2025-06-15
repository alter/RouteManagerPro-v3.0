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
#include <format>

ConfigManager::ConfigManager() : configDirty(false),
lastSaveTime(std::chrono::steady_clock::now()) {
    configPath = std::format("{}\\{}", Utils::GetCurrentDirectory(), Constants::CONFIG_FILE);
    LoadConfig();
    persistThread = std::jthread([this](std::stop_token token) { PersistenceThreadFunc(token); });
}

ConfigManager::~ConfigManager() {
    running = false;
    // std::jthread автоматически вызовет request_stop() и join()

    if (configDirty.load()) {
        Logger::Instance().Info("ConfigManager shutdown: Saving config to disk");
        SaveConfig();
    }
}

void ConfigManager::PersistenceThreadFunc(std::stop_token stopToken) {
    Logger::Instance().Info("ConfigManager persistence thread started");

    try {
        while (!stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown) {
            for (int i = 0; i < 600 && !stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }

            Logger::Instance().Debug("ConfigManager: Periodic backup save");
            SaveConfig();
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error(std::format("ConfigManager::PersistenceThreadFunc exception: {}", e.what()));
    }

    Logger::Instance().Info("ConfigManager persistence thread exiting");
}

ServiceConfig ConfigManager::GetConfig() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return config;
}

void ConfigManager::SetConfig(const ServiceConfig& newConfig) {
    {
        std::lock_guard<std::mutex> lock(configMutex);
        config = newConfig;
    }

    SaveConfig();

    Logger::Instance().Info(std::format("ConfigManager::SetConfig - Updated {} selected processes (saved immediately)",
        config.selectedProcesses.size()));
    for (const auto& proc : config.selectedProcesses) {
        Logger::Instance().Debug(std::format("  - {}", proc));
    }
}

void ConfigManager::SetAIPreloadEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(configMutex);
        config.aiPreloadEnabled = enabled;
    }

    SaveConfig();
}

void ConfigManager::LoadConfig() {
    if (!Utils::FileExists(configPath)) {
        Logger::Instance().Info("ConfigManager::LoadConfig - Config file not found, using defaults");
        config = GetDefaultConfig();
        SaveConfig();
        return;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        Logger::Instance().Error(std::format("ConfigManager::LoadConfig - Failed to open file: {}", configPath));
        config = GetDefaultConfig();
        return;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;

    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        Logger::Instance().Error(std::format("ConfigManager::LoadConfig - Parse error: {}", errors));
        config = GetDefaultConfig();
        return;
    }

    config.gatewayIp = root.get("gatewayIp", config.gatewayIp).asString();
    config.metric = root.get("metric", config.metric).asInt();
    config.startMinimized = root.get("startMinimized", config.startMinimized).asBool();
    config.startWithWindows = root.get("startWithWindows", config.startWithWindows).asBool();
    config.aiPreloadEnabled = root.get("aiPreloadEnabled", config.aiPreloadEnabled).asBool();

    const Json::Value& optimizer = root["optimizerSettings"];
    if (optimizer.isObject()) {
        config.optimizerSettings.minHostsToAggregate =
            optimizer.get("minHostsToAggregate", config.optimizerSettings.minHostsToAggregate).asInt();

        const Json::Value& thresholds = optimizer["wasteThresholds"];
        if (thresholds.isObject()) {
            config.optimizerSettings.wasteThresholds.clear();
            for (const auto& key : thresholds.getMemberNames()) {
                try {
                    int prefix = std::stoi(key);
                    if (prefix >= 0 && prefix <= 32) {
                        config.optimizerSettings.wasteThresholds[prefix] = thresholds[key].asFloat();
                    }
                }
                catch (const std::exception&) {}
            }
        }
    }

    const Json::Value& processes = root["selectedProcesses"];
    if (processes.isArray()) {
        config.selectedProcesses.clear();
        for (const auto& process : processes) {
            config.selectedProcesses.push_back(process.asString());
        }

        Logger::Instance().Info(std::format("ConfigManager::LoadConfig - Loaded {} selected processes",
            config.selectedProcesses.size()));
        for (const auto& proc : config.selectedProcesses) {
            Logger::Instance().Debug(std::format("  - {}", proc));
        }
    }

    file.close();
    configDirty = false;
}

void ConfigManager::SaveConfig() {
    Logger::Instance().Info(std::format("ConfigManager::SaveConfig - Saving configuration with {} selected processes",
        config.selectedProcesses.size()));

    ServiceConfig configCopy;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        configCopy = config;
    }

    Json::Value root;
    root["gatewayIp"] = configCopy.gatewayIp;
    root["metric"] = configCopy.metric;
    root["startMinimized"] = configCopy.startMinimized;
    root["startWithWindows"] = configCopy.startWithWindows;
    root["aiPreloadEnabled"] = configCopy.aiPreloadEnabled;

    Json::Value optimizer;
    optimizer["minHostsToAggregate"] = configCopy.optimizerSettings.minHostsToAggregate;

    Json::Value thresholds;
    for (const auto& [prefix, threshold] : configCopy.optimizerSettings.wasteThresholds) {
        thresholds[std::to_string(prefix)] = threshold;
    }
    optimizer["wasteThresholds"] = thresholds;
    root["optimizerSettings"] = optimizer;

    Json::Value processes(Json::arrayValue);
    for (const auto& process : configCopy.selectedProcesses) {
        processes.append(process);
        Logger::Instance().Debug(std::format("  Saving process: {}", process));
    }
    root["selectedProcesses"] = processes;

    std::ofstream file(configPath + ".tmp");
    if (!file.is_open()) {
        Logger::Instance().Error(std::format("ConfigManager::SaveConfig - Failed to open file: {}.tmp", configPath));
        return;
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(root, &file);
    file.close();

    if (std::filesystem::exists(configPath + ".tmp")) {
        std::filesystem::rename(configPath + ".tmp", configPath);
    }

    configDirty = false;
    lastSaveTime = std::chrono::steady_clock::now();

    Logger::Instance().Debug(std::format("ConfigManager::SaveConfig - Saved to {}", configPath));
}

ServiceConfig ConfigManager::GetDefaultConfig() {
    ServiceConfig defaultConfig;
    defaultConfig.selectedProcesses = {
        "Discord.exe"
    };
    return defaultConfig;
}