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
    std::thread watchThread;
    std::chrono::system_clock::time_point startTime;

    void WatchThreadFunc();
    void CheckMemoryUsage();
    void ForceGarbageCollection();
};