// NetworkMonitor.h
#pragma once
#include <winsock2.h>
#include <windows.h>
#include "../../libs/WinDivert/include/windivert.h"
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include "../common/Models.h"

class RouteController;
class ProcessManager;

class NetworkMonitor {
public:
    NetworkMonitor(RouteController* routeController, ProcessManager* processManager);
    ~NetworkMonitor();

    void Start();
    void Stop();
    bool IsActive() const { return active.load(); }

private:
    RouteController* routeController;
    ProcessManager* processManager;

    HANDLE divertHandle;
    std::atomic<bool> running;
    std::atomic<bool> active;
    std::thread monitorThread;

    struct ConnectionInfo {
        std::string processName;
        std::string remoteIp;
        UINT16 remotePort;
        std::chrono::system_clock::time_point lastSeen;
        size_t packetCount;
    };

    static constexpr size_t MAX_CONNECTIONS = 10000;
    std::unordered_map<UINT64, ConnectionInfo> connections;
    std::mutex connectionsMutex;

    void MonitorThreadFunc();
    void ProcessFlowEvent(const WINDIVERT_ADDRESS& addr);
    void CleanupOldConnections();
    void ForceCleanupOldConnections();
    std::string GetProcessPathFromFlowId(UINT64 flowId, UINT32 processId);
    void LogPerformanceStats();
};