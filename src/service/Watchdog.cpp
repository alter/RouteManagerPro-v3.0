// src/service/Watchdog.cpp
#include <winsock2.h>
#include <windows.h>
#include "Watchdog.h"
#include "ServiceMain.h"
#include "../common/Constants.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <psapi.h>

Watchdog::Watchdog(ServiceMain* svc) : service(svc), running(false),
startTime(std::chrono::system_clock::now()) {
    Logger::Instance().Debug("Watchdog::Watchdog - Constructor called");
}

Watchdog::~Watchdog() {
    Logger::Instance().Debug("Watchdog::~Watchdog - Destructor called");
    Stop();
}

void Watchdog::Start() {
    if (running.load()) return;

    Logger::Instance().Debug("Watchdog::Start - Starting watchdog");
    running = true;
    watchThread = std::jthread([this](std::stop_token token) { WatchThreadFunc(token); });
}

void Watchdog::Stop() {
    Logger::Instance().Debug("Watchdog::Stop - Stopping watchdog");
    running = false;
    // std::jthread автоматически вызовет request_stop() и join()
}

size_t Watchdog::GetMemoryUsageMB() const {
    try {
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            return pmc.WorkingSetSize / (1024 * 1024);
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("Watchdog::GetMemoryUsageMB - Exception: " + std::string(e.what()));
    }
    return 0;
}

std::chrono::seconds Watchdog::GetUptime() const {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - startTime);
}

void Watchdog::WatchThreadFunc(std::stop_token stopToken) {
    Logger::Instance().Debug("Watchdog::WatchThreadFunc - Started");

    while (!stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown) {
        try {
            CheckMemoryUsage();
        }
        catch (const std::exception& e) {
            Logger::Instance().Error("Watchdog::WatchThreadFunc - Exception: " + std::string(e.what()));
        }

        for (int i = 0; i < Constants::WATCHDOG_INTERVAL_SEC * 10; i++) {
            if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    Logger::Instance().Debug("Watchdog::WatchThreadFunc - Exiting");
}

void Watchdog::CheckMemoryUsage() {
    size_t memoryMB = GetMemoryUsageMB();

    if (memoryMB > Constants::MAX_MEMORY_MB) {
        Logger::Instance().Warning("Watchdog::CheckMemoryUsage - High memory usage: " + std::to_string(memoryMB) + "MB");
        ForceGarbageCollection();

        std::this_thread::sleep_for(std::chrono::seconds(5));

        if (GetMemoryUsageMB() > Constants::MAX_MEMORY_MB) {
            Logger::Instance().Error("Watchdog::CheckMemoryUsage - Memory still high after GC");
        }
    }
}

void Watchdog::ForceGarbageCollection() {
    Logger::Instance().Debug("Watchdog::ForceGarbageCollection - Starting");

    SetProcessWorkingSetSize(GetCurrentProcess(), -1, -1);

    HANDLE heap = GetProcessHeap();
    if (heap) {
        HeapCompact(heap, 0);
    }

    Logger::Instance().Debug("Watchdog::ForceGarbageCollection - Completed");
}