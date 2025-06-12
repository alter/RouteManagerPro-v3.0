// src/service/NetworkMonitor.cpp
#include "NetworkMonitor.h"
#include "RouteController.h"
#include "ProcessManager.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <sstream>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "WinDivert.lib")

NetworkMonitor::NetworkMonitor(RouteController* rc, ProcessManager* pm)
    : routeController(rc), processManager(pm),
    divertHandle(INVALID_HANDLE_VALUE), running(false), active(false) {
    Logger::Instance().Info("NetworkMonitor created");
}

NetworkMonitor::~NetworkMonitor() {
    Stop();
}

void NetworkMonitor::Start() {
    if (running.load()) return;

    Logger::Instance().Info("Starting NetworkMonitor");

    divertHandle = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 0, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    if (divertHandle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        Logger::Instance().Error("Failed to open WinDivert handle: " + std::to_string(error));
        return;
    }

    Logger::Instance().Info("WinDivert handle opened successfully");

    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_LENGTH, 16384);
    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_TIME, 50);
    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_SIZE, 8388608);

    running = true;
    monitorThread = std::thread(&NetworkMonitor::MonitorThreadFunc, this);

    Logger::Instance().Info("NetworkMonitor started - monitoring FLOW events");
}

void NetworkMonitor::Stop() {
    Logger::Instance().Info("NetworkMonitor::Stop called");

    running = false;
    active = false;

    if (divertHandle != INVALID_HANDLE_VALUE) {
        Logger::Instance().Info("Shutting down WinDivert handle");

        if (!WinDivertShutdown(divertHandle, WINDIVERT_SHUTDOWN_BOTH)) {
            DWORD error = GetLastError();
            Logger::Instance().Warning("WinDivertShutdown failed: " + std::to_string(error));
        }

        if (monitorThread.joinable()) {
            Logger::Instance().Info("Waiting for monitor thread to complete");

            if (monitorThread.joinable()) {
                monitorThread.join();
                Logger::Instance().Info("Monitor thread joined successfully");
            }
        }

        Logger::Instance().Info("Closing WinDivert handle");
        WinDivertClose(divertHandle);
        divertHandle = INVALID_HANDLE_VALUE;
    }

    Logger::Instance().Info("NetworkMonitor stopped");
}

void NetworkMonitor::MonitorThreadFunc() {
    WINDIVERT_ADDRESS addr;

    active = true;
    auto lastCleanup = std::chrono::steady_clock::now();
    int eventCount = 0;

    Logger::Instance().Info("Monitor thread started - waiting for FLOW events");

    while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
        UINT recvLen = 0;

        if (!WinDivertRecv(divertHandle, NULL, 0, &recvLen, &addr)) {
            DWORD error = GetLastError();

            if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                Logger::Instance().Info("Monitor thread: Shutdown detected during recv, exiting");
                break;
            }

            if (error == ERROR_NO_DATA) {
                Logger::Instance().Info("Monitor thread: WinDivert handle shut down (ERROR_NO_DATA)");
                break;
            }
            else if (error == ERROR_INVALID_PARAMETER) {
                Logger::Instance().Info("Monitor thread: WinDivert handle closed");
                break;
            }
            else if (error != ERROR_INSUFFICIENT_BUFFER) {
                Logger::Instance().Error("WinDivertRecv failed: " + std::to_string(error));
                if (error == ERROR_INVALID_HANDLE) {
                    break;
                }
            }
            continue;
        }

        if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
            Logger::Instance().Info("Monitor thread: Shutdown detected after recv, exiting");
            break;
        }

        if (addr.Layer == WINDIVERT_LAYER_FLOW) {
            if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED ||
                addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
                eventCount++;
                if (eventCount <= 10 || eventCount % 100 == 0) {
                    Logger::Instance().Info("Processing FLOW event #" + std::to_string(eventCount));
                }
                ProcessFlowEvent(addr);
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup).count() > 60) {
            CleanupOldConnections();
            lastCleanup = now;
        }
    }

    active = false;
    Logger::Instance().Info("Monitor thread exiting cleanly after processing " + std::to_string(eventCount) + " events");
}

void NetworkMonitor::ProcessFlowEvent(const WINDIVERT_ADDRESS& addr) {
    std::string processPath = GetProcessPathFromFlowId(0, addr.Flow.ProcessId);
    if (processPath.empty()) return;

    std::string processName = Utils::GetProcessNameFromPath(processPath);

    char localStr[INET6_ADDRSTRLEN], remoteStr[INET6_ADDRSTRLEN];
    WinDivertHelperFormatIPv6Address(addr.Flow.LocalAddr, localStr, sizeof(localStr));
    WinDivertHelperFormatIPv6Address(addr.Flow.RemoteAddr, remoteStr, sizeof(remoteStr));

    std::string remoteIp = remoteStr;

    if (remoteIp.substr(0, 7) == "::ffff:") {
        remoteIp = remoteIp.substr(7);
    }
    else if (remoteIp.find(':') != std::string::npos) {
        Logger::Instance().Debug("Skipping IPv6 address: " + remoteIp + " for process: " + processName);
        return;
    }

    std::stringstream logMsg;
    logMsg << "Flow event: " << (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED ? "ESTABLISHED" : "DELETED")
        << " Process: " << processName << " (" << addr.Flow.ProcessId << ")"
        << " Remote: " << remoteIp << ":" << ntohs(addr.Flow.RemotePort)
        << " Protocol: " << (int)addr.Flow.Protocol;
    Logger::Instance().Info(logMsg.str());

    if (Utils::IsPrivateIP(remoteIp)) {
        Logger::Instance().Debug("Skipping private IP: " + remoteIp);
        return;
    }

    bool isSelected = processManager->IsProcessSelected(processName);
    if (!isSelected) {
        Logger::Instance().Debug("Process not selected: " + processName);
        return;
    }

    Logger::Instance().Info("Selected process detected: " + processName + " -> " + remoteIp);

    if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
        std::lock_guard<std::mutex> lock(connectionsMutex);

        UINT64 flowId = ((UINT64)addr.Flow.ProcessId << 32) |
            ((UINT64)addr.Flow.LocalPort << 16) |
            addr.Flow.RemotePort;

        connections[flowId] = {
            processName,
            remoteIp,
            ntohs(addr.Flow.RemotePort),
            std::chrono::system_clock::now(),
            0
        };

        if (routeController) {
            Logger::Instance().Info("Adding route IMMEDIATELY for " + remoteIp + " (process: " + processName + ")");
            routeController->AddRoute(remoteIp, processName);
        }
    }
    else if (addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        UINT64 flowId = ((UINT64)addr.Flow.ProcessId << 32) |
            ((UINT64)addr.Flow.LocalPort << 16) |
            addr.Flow.RemotePort;
        connections.erase(flowId);
        Logger::Instance().Debug("Flow deleted for " + processName);
    }
}

void NetworkMonitor::CleanupOldConnections() {
    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto now = std::chrono::system_clock::now();
    int cleaned = 0;

    for (auto it = connections.begin(); it != connections.end();) {
        auto duration = std::chrono::duration_cast<std::chrono::hours>(now - it->second.lastSeen);
        if (duration.count() >= Constants::CONNECTION_CLEANUP_HOURS) {
            it = connections.erase(it);
            cleaned++;
        }
        else {
            ++it;
        }
    }

    if (cleaned > 0) {
        Logger::Instance().Info("Cleaned up " + std::to_string(cleaned) + " old connections");
    }
}

std::string NetworkMonitor::GetProcessPathFromFlowId(UINT64 flowId, UINT32 processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return "";

    char path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameA(process, 0, path, &size)) {
        CloseHandle(process);
        return "";
    }

    CloseHandle(process);
    return std::string(path);
}