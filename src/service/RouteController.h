// src/service/RouteController.h (обновленная версия)
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <condition_variable>
#include <winsock2.h>
#include <netioapi.h>
#include "../common/Models.h"
#include "../common/Result.h"
#include "RouteOptimizer.h"

struct SystemRoute {
    uint32_t address;
    uint32_t mask;
    int prefixLength;
    std::string ipString;
};

class RouteController {
public:
    RouteController(const ServiceConfig& config);
    ~RouteController();

    // Public API
    bool AddRoute(const std::string& ip, const std::string& processName);
    bool AddRouteWithMask(const std::string& ip, int prefixLength, const std::string& processName);
    bool RemoveRoute(const std::string& ip);
    bool RemoveRouteWithMask(const std::string& ip, int prefixLength);
    void CleanupAllRoutes();
    void CleanupOldRoutes();
    size_t GetRouteCount() const;
    std::vector<RouteInfo> GetActiveRoutes() const;
    void PreloadAIRoutes();
    ServiceConfig GetConfig() const { return config; }
    void UpdateConfig(const ServiceConfig& newConfig);
    void RunOptimizationManual();

    void SyncWithSystemTable();
    void PerformFullCleanup();
    void CleanupRedundantRoutes();

    const RouteError& GetLastError() const { return lastError; }

private:
    ServiceConfig config;
    std::unordered_map<std::string, std::unique_ptr<RouteInfo>> routes;
    mutable std::shared_mutex routesMutex;  // Read-write lock для маршрутов
    std::atomic<bool> running;

    std::jthread verifyThread;
    std::jthread persistThread;
    std::jthread optimizationThread;

    std::unique_ptr<RouteOptimizer> optimizer;
    std::chrono::steady_clock::time_point lastOptimizationTime;
    std::condition_variable optimizationCV;
    std::mutex optimizationMutex;

    NET_IFINDEX cachedInterfaceIndex;
    mutable std::shared_mutex interfaceCacheMutex;  // Read-write lock для кэша интерфейса

    std::atomic<bool> routesDirty{ false };
    std::chrono::steady_clock::time_point lastSaveTime;
    static constexpr auto SAVE_INTERVAL = std::chrono::minutes(10);

    // Error tracking
    mutable RouteError lastError;
    mutable std::mutex errorMutex;

    // Internal methods
    bool AddSystemRoute(const std::string& ip);
    bool AddSystemRouteWithMask(const std::string& ip, int prefixLength);
    bool AddSystemRouteOldAPI(const std::string& ip);
    bool AddSystemRouteOldAPIWithMask(const std::string& ip, int prefixLength);
    bool RemoveSystemRoute(const std::string& ip, const std::string& gatewayIp);
    bool RemoveSystemRouteWithMask(const std::string& ip, int prefixLength, const std::string& gatewayIp);

    Result<void> AddSystemRouteEx(const std::string& ip, int prefixLength);
    Result<void> RemoveSystemRouteEx(const std::string& ip, int prefixLength, const std::string& gatewayIp);

    void SetLastError(const RouteError& error);
    void ClearLastError();

    void VerifyRoutesThreadFunc(std::stop_token stopToken);
    void PersistenceThreadFunc(std::stop_token stopToken);
    void OptimizationThreadFunc(std::stop_token stopToken);

    void SaveRoutesToDisk();
    void SaveRoutesToDiskAsync();
    void LoadRoutesFromDisk();

    bool IsGatewayReachable();
    void InvalidateInterfaceCache();
    void MigrateExistingRoutes(const std::string& oldGateway, const std::string& newGateway);
    void RunOptimization();
    void ApplyOptimizationPlan(const OptimizationPlan& plan);
    bool IsIPCoveredByExistingRoute(const std::string& ip);
    uint32_t IPToUInt(const std::string& ip);
    static constexpr uint32_t CreateMask(int prefixLength);
    void NotifyUIRouteCountChanged();

    std::vector<SystemRoute> GetSystemRoutesForGateway();
    std::vector<SystemRoute> GetSystemRoutesOldAPI();
    void RemoveRedundantSystemRoutes(const std::vector<HostRoute>& hostRoutes,
        const std::vector<SystemRoute>& aggregatedRoutes);
    int CountBits(uint32_t mask);

    struct PreloadService {
        std::string name;
        bool enabled;
        std::vector<std::string> ranges;
    };

    std::vector<PreloadService> LoadPreloadConfig();
    void CreateDefaultPreloadConfig(const std::string& path);
    std::vector<PreloadService> GetDefaultPreloadServices();
    bool AddCIDRRoute(const std::string& cidr, const std::string& service);
};