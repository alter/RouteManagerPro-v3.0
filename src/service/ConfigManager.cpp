// src/service/ConfigManager.cpp
#include "ConfigManager.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include <fstream>
#include <json/json.h>

ConfigManager::ConfigManager() {
    configPath = Utils::GetCurrentDirectory() + "\\" + Constants::CONFIG_FILE;
    LoadConfig();
}

ConfigManager::~ConfigManager() {
    SaveConfig();
}

ServiceConfig ConfigManager::GetConfig() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return config;
}

void ConfigManager::SetConfig(const ServiceConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;
    SaveConfig();

    Logger::Instance().Info("ConfigManager::SetConfig - Saved " +
        std::to_string(config.selectedProcesses.size()) + " selected processes");
    for (const auto& proc : config.selectedProcesses) {
        Logger::Instance().Debug("  - " + proc);
    }
}

void ConfigManager::SetAIPreloadEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(configMutex);
    config.aiPreloadEnabled = enabled;
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
}

void ConfigManager::SaveConfig() {
    Logger::Instance().Info("ConfigManager::SaveConfig - Saving configuration with " +
        std::to_string(config.selectedProcesses.size()) + " selected processes");

    Json::Value root;
    root["gatewayIp"] = config.gatewayIp;
    root["metric"] = config.metric;
    root["startMinimized"] = config.startMinimized;
    root["startWithWindows"] = config.startWithWindows;
    root["aiPreloadEnabled"] = config.aiPreloadEnabled;

    Json::Value processes(Json::arrayValue);
    for (const auto& process : config.selectedProcesses) {
        processes.append(process);
        Logger::Instance().Debug("  Saving process: " + process);
    }
    root["selectedProcesses"] = processes;

    std::ofstream file(configPath);
    if (!file.is_open()) {
        Logger::Instance().Error("ConfigManager::SaveConfig - Failed to open file: " + configPath);
        return;
    }

    Json::StreamWriterBuilder builder;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(root, &file);
    file.close();

    Logger::Instance().Debug("ConfigManager::SaveConfig - Saved to " + configPath);
}

ServiceConfig ConfigManager::GetDefaultConfig() {
    ServiceConfig defaultConfig;
    defaultConfig.selectedProcesses = {
        "Discord.exe"
    };
    return defaultConfig;
}