// src/service/NetworkMonitor.h
#pragma once
#include <winsock2.h>
#include <windows.h>
#include "../../libs/WinDivert/include/windivert.h"
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
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

    // Connection tracking limits
    static constexpr size_t MAX_CONNECTIONS = 10000;
    static constexpr size_t CLEANUP_TRIGGER_PERCENT = 80;
    static constexpr auto AGGRESSIVE_CLEANUP_AGE = std::chrono::minutes(30);

    std::unordered_map<UINT64, ConnectionInfo> connections;
    std::mutex connectionsMutex;

    void MonitorThreadFunc();
    void ProcessFlowEvent(const WINDIVERT_ADDRESS& addr);
    void HandleNewProcess(DWORD pid, const std::string& remoteIp, WINDIVERT_EVENT event);
    bool VerifyProcessIdentity(DWORD pid, const FILETIME& expectedTime);
    void CleanupOldConnections();
    void ForceCleanupOldConnections();
    std::string GetProcessPathFromFlowId(UINT64 flowId, UINT32 processId);
    void LogDetailedStats();
};