// src/service/ConfigManager.h
#pragma once
#include <string>
#include <mutex>
#include "../common/Models.h"

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    ServiceConfig GetConfig() const;
    void SetConfig(const ServiceConfig& config);
    void SetAIPreloadEnabled(bool enabled);

private:
    mutable std::mutex configMutex;
    ServiceConfig config;
    std::string configPath;

    void LoadConfig();
    void SaveConfig();
    ServiceConfig GetDefaultConfig();
};