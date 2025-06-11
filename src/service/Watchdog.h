// src/service/Watchdog.h
#pragma once
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <mutex>

class ServiceMain;

class Watchdog {
public:
    Watchdog(ServiceMain* service);
    ~Watchdog();

    void Start();
    void Stop();
    size_t GetMemoryUsageMB() const;
    std::chrono::seconds GetUptime() const;

private:
    ServiceMain* service;
    std::atomic<bool> running;
    std::atomic<bool> isStoppingFlag;
    std::thread watchThread;
    std::chrono::system_clock::time_point startTime;

    struct ComponentHealth {
        std::chrono::system_clock::time_point lastActivity;
        int restartCount;
        bool isHealthy;
    };

    std::unordered_map<std::string, ComponentHealth> components;
    mutable std::mutex componentsMutex;

    void WatchThreadFunc();
    void CheckMemoryUsage();
    void CheckComponentHealth();
    void RestartComponent(const std::string& name);
    void ForceGarbageCollection();
};