// src/service/ConfigManager.h
#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
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

    // Persistence optimization
    std::atomic<bool> configDirty{ false };
    std::chrono::steady_clock::time_point lastSaveTime;
    static constexpr auto SAVE_INTERVAL = std::chrono::minutes(10);
    std::atomic<bool> running{ true };
    std::jthread persistThread;

    void LoadConfig();
    void SaveConfig();
    ServiceConfig GetDefaultConfig();
    void PersistenceThreadFunc(std::stop_token stopToken);
};