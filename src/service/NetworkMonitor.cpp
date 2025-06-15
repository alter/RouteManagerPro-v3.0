// NetworkMonitor.cpp
#include "NetworkMonitor.h"
#include "RouteController.h"
#include "ProcessManager.h"
#include "PerformanceMonitor.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include "../common/WinHandles.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <format>
#include <chrono>
#include <atomic>

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
    if (running.load(std::memory_order_acquire)) return;

    Logger::Instance().Info("Starting NetworkMonitor");

    divertHandle = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 0, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    if (divertHandle == INVALID_HANDLE_VALUE) {
        DWORD error = ::GetLastError();
        Logger::Instance().Error(std::format("Failed to open WinDivert handle: {}", error));
        return;
    }

    Logger::Instance().Info("WinDivert handle opened successfully");

    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_LENGTH, 32768);
    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_TIME, 1);
    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_SIZE, 16777216);

    running.store(true, std::memory_order_release);
    monitorThread = std::thread(&NetworkMonitor::MonitorThreadFunc, this);

    Logger::Instance().Info("NetworkMonitor started - monitoring FLOW events");
}

void NetworkMonitor::Stop() {
    Logger::Instance().Info("NetworkMonitor::Stop called");

    running.store(false, std::memory_order_release);
    active.store(false, std::memory_order_release);

    if (divertHandle != INVALID_HANDLE_VALUE) {
        Logger::Instance().Info("Shutting down WinDivert handle");

        if (!WinDivertShutdown(divertHandle, WINDIVERT_SHUTDOWN_BOTH)) {
            DWORD error = ::GetLastError();
            Logger::Instance().Warning(std::format("WinDivertShutdown failed: {}", error));
        }

        if (monitorThread.joinable()) {
            Logger::Instance().Info("Waiting for monitor thread to complete");
            monitorThread.join();
            Logger::Instance().Info("Monitor thread joined successfully");
        }

        Logger::Instance().Info("Closing WinDivert handle");
        WinDivertClose(divertHandle);
        divertHandle = INVALID_HANDLE_VALUE;
    }

    Logger::Instance().Info("NetworkMonitor stopped");
}

void NetworkMonitor::MonitorThreadFunc() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DWORD_PTR mask = 1;
    SetThreadAffinityMask(GetCurrentThread(), mask);

    WINDIVERT_ADDRESS addr;

    active.store(true, std::memory_order_release);
    auto lastCleanup = std::chrono::steady_clock::now();
    auto lastStats = std::chrono::steady_clock::now();
    int eventCount = 0;

    Logger::Instance().Info("Monitor thread started - waiting for FLOW events");

    while (running.load(std::memory_order_acquire) && !ShutdownCoordinator::Instance().isShuttingDown) {
        UINT recvLen = 0;

        if (!WinDivertRecv(divertHandle, NULL, 0, &recvLen, &addr)) {
            DWORD error = ::GetLastError();

            if (!running.load(std::memory_order_acquire) || ShutdownCoordinator::Instance().isShuttingDown) {
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
                Logger::Instance().Error(std::format("WinDivertRecv failed: {}", error));
                if (error == ERROR_INVALID_HANDLE) {
                    break;
                }
            }
            continue;
        }

        if (!running.load(std::memory_order_acquire) || ShutdownCoordinator::Instance().isShuttingDown) {
            Logger::Instance().Info("Monitor thread: Shutdown detected after recv, exiting");
            break;
        }

        if (addr.Layer == WINDIVERT_LAYER_FLOW) {
            if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED ||
                addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
                eventCount++;
                if (eventCount <= 10 || eventCount % 100 == 0) {
                    Logger::Instance().Info(std::format("Processing FLOW event #{}", eventCount));
                }
                ProcessFlowEvent(addr);
            }
        }

        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup).count() > 120) {
            CleanupOldConnections();
            lastCleanup = now;
        }

        if (std::chrono::duration_cast<std::chrono::minutes>(now - lastStats).count() >= 5) {
            LogPerformanceStats();
            lastStats = now;
        }
    }

    active.store(false, std::memory_order_release);
    Logger::Instance().Info(std::format("Monitor thread exiting cleanly after processing {} events", eventCount));
}

void NetworkMonitor::ProcessFlowEvent(const WINDIVERT_ADDRESS& addr) {
    PERF_TIMER("NetworkMonitor::ProcessFlowEvent");

    auto eventStartTime = std::chrono::high_resolution_clock::now();

    bool isSelected = processManager->IsSelectedProcessByPid(addr.Flow.ProcessId);
    if (!isSelected) {
        PERF_COUNT("NetworkMonitor.FlowEvent.Filtered");
        return;
    }

    auto cachedInfo = processManager->GetCachedInfo(addr.Flow.ProcessId);
    std::string processName = cachedInfo.has_value() ?
        Utils::WStringToString(cachedInfo->name) : "Unknown";

    thread_local char localStr[INET6_ADDRSTRLEN];
    thread_local char remoteStr[INET6_ADDRSTRLEN];

    WinDivertHelperFormatIPv6Address(addr.Flow.LocalAddr, localStr, sizeof(localStr));
    WinDivertHelperFormatIPv6Address(addr.Flow.RemoteAddr, remoteStr, sizeof(remoteStr));

    std::string remoteIp = remoteStr;

    if (remoteIp.starts_with("::ffff:")) {
        remoteIp = remoteIp.substr(7);
    }
    else if (remoteIp.contains(':')) {
        PERF_COUNT("NetworkMonitor.FlowEvent.IPv6Skipped");
        Logger::Instance().Debug(std::format("Skipping IPv6 address: {} for process: {}", remoteIp, processName));
        return;
    }

    Logger::Instance().Info(std::format("Flow event: {} Process: {} ({}) Remote: {}:{} Protocol: {}",
        addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED ? "ESTABLISHED" : "DELETED",
        processName, addr.Flow.ProcessId, remoteIp, ntohs(addr.Flow.RemotePort),
        static_cast<int>(addr.Flow.Protocol)));

    if (Utils::IsPrivateIP(remoteIp)) {
        PERF_COUNT("NetworkMonitor.FlowEvent.PrivateIPSkipped");
        Logger::Instance().Debug(std::format("Skipping private IP: {}", remoteIp));
        return;
    }

    Logger::Instance().Info(std::format("Selected process detected: {} -> {}", processName, remoteIp));

    if (addr.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
        PERF_COUNT("NetworkMonitor.FlowEvent.Established");

        // IMMEDIATE PROCESSING - NO BATCHING!
        Logger::Instance().Info(std::format("Adding route IMMEDIATELY for {} (process: {})", remoteIp, processName));

        auto routeAddStart = std::chrono::high_resolution_clock::now();
        bool success = routeController->AddRoute(remoteIp, processName);
        auto routeAddEnd = std::chrono::high_resolution_clock::now();

        auto totalTime = std::chrono::duration_cast<std::chrono::microseconds>(routeAddEnd - routeAddStart);
        PerformanceMonitor::Instance().RecordOperation("RouteAddLatency", totalTime);

        if (success) {
            PERF_COUNT("NetworkMonitor.RouteAdded");
            Logger::Instance().Info(std::format("Route added successfully for {}: {}µs", remoteIp, totalTime.count()));
        }
        else {
            Logger::Instance().Error(std::format("Failed to add route for {}", remoteIp));
        }

        // Update connections tracking
        std::lock_guard<std::mutex> lock(connectionsMutex);

        if (connections.size() >= MAX_CONNECTIONS) {
            PERF_COUNT("NetworkMonitor.ConnectionLimitReached");
            Logger::Instance().Warning(std::format("Connection limit reached ({}), cleaning up old connections", MAX_CONNECTIONS));
            ForceCleanupOldConnections();
        }

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
    }
    else if (addr.Event == WINDIVERT_EVENT_FLOW_DELETED) {
        PERF_COUNT("NetworkMonitor.FlowEvent.Deleted");
        std::lock_guard<std::mutex> lock(connectionsMutex);
        UINT64 flowId = ((UINT64)addr.Flow.ProcessId << 32) |
            ((UINT64)addr.Flow.LocalPort << 16) |
            addr.Flow.RemotePort;
        connections.erase(flowId);
        Logger::Instance().Debug(std::format("Flow deleted for {}", processName));
    }

    auto eventEndTime = std::chrono::high_resolution_clock::now();
    auto eventTotalTime = std::chrono::duration_cast<std::chrono::microseconds>(eventEndTime - eventStartTime);
    Logger::Instance().Debug(std::format("Total event processing time: {}µs", eventTotalTime.count()));
}

void NetworkMonitor::CleanupOldConnections() {
    PERF_TIMER("NetworkMonitor::CleanupOldConnections");

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
        PERF_COUNT("NetworkMonitor.ConnectionsCleaned");
        Logger::Instance().Info(std::format("Cleaned up {} old connections", cleaned));
    }
}

void NetworkMonitor::ForceCleanupOldConnections() {
    auto now = std::chrono::system_clock::now();
    int cleaned = 0;

    for (auto it = connections.begin(); it != connections.end();) {
        auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - it->second.lastSeen);
        if (duration.count() >= 30) {
            it = connections.erase(it);
            cleaned++;
        }
        else {
            ++it;
        }
    }

    if (connections.size() > MAX_CONNECTIONS * 0.8) {
        std::vector<std::pair<UINT64, std::chrono::system_clock::time_point>> sortedConnections;
        sortedConnections.reserve(connections.size());

        for (const auto& [id, info] : connections) {
            sortedConnections.emplace_back(id, info.lastSeen);
        }

        size_t toRemove = connections.size() - static_cast<size_t>(MAX_CONNECTIONS * 0.8);
        std::partial_sort(sortedConnections.begin(),
            sortedConnections.begin() + toRemove,
            sortedConnections.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        for (size_t i = 0; i < toRemove; ++i) {
            connections.erase(sortedConnections[i].first);
            cleaned++;
        }
    }

    Logger::Instance().Info(std::format("Force cleanup removed {} connections", cleaned));
}

std::string NetworkMonitor::GetProcessPathFromFlowId(UINT64 flowId, UINT32 processId) {
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId), HandleDeleter{});
    if (!process) return "";

    char path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameA(process.get(), 0, path, &size)) {
        return "";
    }

    return std::string(path);
}

void NetworkMonitor::LogPerformanceStats() {
    auto report = PerformanceMonitor::Instance().GetReport();

    Logger::Instance().Info("=== NetworkMonitor Performance Stats ===");

    for (const auto& [name, count] : report.counters) {
        if (name.starts_with("NetworkMonitor.")) {
            Logger::Instance().Info(std::format("{}: {}", name, count));
        }
    }

    for (const auto& op : report.operations) {
        if (op.name.starts_with("NetworkMonitor::") || op.name == "RouteAddLatency") {
            Logger::Instance().Info(std::format("{}: {} calls, avg: {}us, p95: {}us",
                op.name, op.count, op.avgTime.count(), op.p95Time.count()));
        }
    }
}