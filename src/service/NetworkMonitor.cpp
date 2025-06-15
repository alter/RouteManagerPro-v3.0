// src/service/NetworkMonitor.cpp
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

    // Увеличиваем размеры буферов для лучшей производительности
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
    // Привязываем поток к процессору для лучшей производительности
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // Пытаемся привязать к конкретному CPU для уменьшения cache misses
    DWORD_PTR mask = 1; // CPU 0
    SetThreadAffinityMask(GetCurrentThread(), mask);

    EventBatch eventBatch;
    WINDIVERT_ADDRESS addr;

    active.store(true, std::memory_order_release);
    auto lastCleanup = std::chrono::steady_clock::now();
    auto lastStats = std::chrono::steady_clock::now();
    auto lastBatchFlush = std::chrono::steady_clock::now();
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
                ProcessFlowEvent(addr, eventBatch);
            }
        }

        auto now = std::chrono::steady_clock::now();

        // Флушим батч если он полный или прошло достаточно времени
        if (eventBatch.isFull() ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBatchFlush).count() > 100) {
            FlushEventBatch(eventBatch);
            lastBatchFlush = now;
        }

        // Реже выполняем cleanup
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup).count() > 120) {
            CleanupOldConnections();
            lastCleanup = now;
        }

        // Log performance stats every 5 minutes
        if (std::chrono::duration_cast<std::chrono::minutes>(now - lastStats).count() >= 5) {
            LogPerformanceStats();
            lastStats = now;
        }
    }

    // Флушим оставшиеся события
    if (!eventBatch.isEmpty()) {
        FlushEventBatch(eventBatch);
    }

    active.store(false, std::memory_order_release);
    Logger::Instance().Info(std::format("Monitor thread exiting cleanly after processing {} events", eventCount));
}

void NetworkMonitor::ProcessFlowEvent(const WINDIVERT_ADDRESS& addr, EventBatch& batch) {
    PERF_TIMER("NetworkMonitor::ProcessFlowEvent");

    // Запоминаем время начала обработки события
    auto eventStartTime = std::chrono::high_resolution_clock::now();

    bool isSelected = processManager->IsSelectedProcessByPid(addr.Flow.ProcessId);
    if (!isSelected) {
        PERF_COUNT("NetworkMonitor.FlowEvent.Filtered");
        return;
    }

    auto cachedInfo = processManager->GetCachedInfo(addr.Flow.ProcessId);
    std::string processName = cachedInfo.has_value() ?
        Utils::WStringToString(cachedInfo->name) : "Unknown";

    // Используем thread_local буфер для конвертации адресов
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

        // Сохраняем время начала для этого маршрута
        {
            std::lock_guard<std::mutex> lock(routeTimingMutex);
            routeAddStartTimes[remoteIp] = eventStartTime;
        }

        // Добавляем в батч вместо немедленной обработки
        batch.add(remoteIp, processName);

        std::lock_guard<std::mutex> lock(connectionsMutex);

        // Check connection limit
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
}

void NetworkMonitor::FlushEventBatch(EventBatch& batch) {
    if (batch.isEmpty()) return;

    PERF_TIMER("NetworkMonitor::FlushEventBatch");

    if (routeController) {
        for (size_t i = 0; i < batch.count; ++i) {
            const auto& [ip, process] = batch.events[i];

            auto routeAddStart = std::chrono::high_resolution_clock::now();

            // Получаем время начала для этого маршрута
            {
                std::lock_guard<std::mutex> lock(routeTimingMutex);
                auto it = routeAddStartTimes.find(ip);
                if (it != routeAddStartTimes.end()) {
                    routeAddStart = it->second;
                    routeAddStartTimes.erase(it);
                }
            }

            Logger::Instance().Info(std::format("Adding route (batched) for {} (process: {})", ip, process));

            bool success = routeController->AddRoute(ip, process);

            if (success) {
                auto routeAddEnd = std::chrono::high_resolution_clock::now();
                auto totalTime = std::chrono::duration_cast<std::chrono::microseconds>(routeAddEnd - routeAddStart);

                // Записываем метрику времени добавления маршрута
                PerformanceMonitor::Instance().RecordOperation("RouteAddLatency", totalTime);

                PERF_COUNT("NetworkMonitor.RouteAdded");

                // Логируем время добавления маршрута
                if (totalTime.count() > 1000) { // Если больше 1мс
                    Logger::Instance().Warning(std::format("Route add latency for {}: {}µs", ip, totalTime.count()));
                }
                else {
                    Logger::Instance().Debug(std::format("Route add latency for {}: {}µs", ip, totalTime.count()));
                }

                // Обновляем статистику
                {
                    std::lock_guard<std::mutex> lock(routeTimingMutex);
                    totalRoutesAdded.fetch_add(1, std::memory_order_relaxed);
                    totalRouteAddTime += totalTime;

                    if (totalTime < minRouteAddTime || minRouteAddTime.count() == 0) {
                        minRouteAddTime = totalTime;
                    }
                    if (totalTime > maxRouteAddTime) {
                        maxRouteAddTime = totalTime;
                    }
                }
            }
        }
    }

    batch.clear();
}

void NetworkMonitor::CleanupOldConnections() {
    PERF_TIMER("NetworkMonitor::CleanupOldConnections");

    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto now = std::chrono::system_clock::now();
    int cleaned = 0;

    // Используем std::erase_if для более эффективного удаления
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

    // More aggressive cleanup - remove connections older than 30 minutes
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

    // If still too many, remove oldest connections
    if (connections.size() > MAX_CONNECTIONS * 0.8) {
        // Используем partial_sort вместо полной сортировки
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
    // Используем UniqueHandle для автоматического закрытия
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

    // Логируем статистику времени добавления маршрутов
    {
        std::lock_guard<std::mutex> lock(routeTimingMutex);
        uint64_t routesAdded = totalRoutesAdded.load(std::memory_order_relaxed);
        if (routesAdded > 0) {
            auto avgTime = totalRouteAddTime / routesAdded;
            Logger::Instance().Info(std::format("Route Add Statistics: {} routes added", routesAdded));
            Logger::Instance().Info(std::format("  Average time: {}µs", avgTime.count()));
            Logger::Instance().Info(std::format("  Min time: {}µs", minRouteAddTime.count()));
            Logger::Instance().Info(std::format("  Max time: {}µs", maxRouteAddTime.count()));
        }
    }
}