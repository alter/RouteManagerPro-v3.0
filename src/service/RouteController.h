// src/service/RouteController.h
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <condition_variable>
#include <winsock2.h>
#include <netioapi.h>
#include "../common/Models.h"
#include "RouteOptimizer.h"

class RouteController {
public:
    RouteController(const ServiceConfig& config);
    ~RouteController();

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

private:
    ServiceConfig config;
    std::unordered_map<std::string, std::unique_ptr<RouteInfo>> routes;
    mutable std::mutex routesMutex;
    std::atomic<bool> running;

    // Threads
    std::thread verifyThread;
    std::thread persistThread;
    std::thread optimizationThread;

    // Optimization
    std::unique_ptr<RouteOptimizer> optimizer;
    std::chrono::steady_clock::time_point lastOptimizationTime;
    std::condition_variable optimizationCV;
    std::mutex optimizationMutex;

    NET_IFINDEX cachedInterfaceIndex;
    std::mutex interfaceCacheMutex;

    // Persistence
    std::atomic<bool> routesDirty{ false };
    std::chrono::steady_clock::time_point lastSaveTime;
    static constexpr auto SAVE_INTERVAL = std::chrono::minutes(10);

    // System route management
    bool AddSystemRoute(const std::string& ip);
    bool AddSystemRouteWithMask(const std::string& ip, int prefixLength);
    bool AddSystemRouteOldAPI(const std::string& ip);
    bool AddSystemRouteOldAPIWithMask(const std::string& ip, int prefixLength);
    bool RemoveSystemRoute(const std::string& ip, const std::string& gatewayIp);
    bool RemoveSystemRouteWithMask(const std::string& ip, int prefixLength, const std::string& gatewayIp);

    // Thread functions
    void VerifyRoutesThreadFunc();
    void PersistenceThreadFunc();
    void OptimizationThreadFunc();

    // Persistence
    void SaveRoutesToDisk();
    void SaveRoutesToDiskAsync();
    void LoadRoutesFromDisk();

    // Helpers
    bool IsGatewayReachable();
    void InvalidateInterfaceCache();
    void MigrateExistingRoutes(const std::string& oldGateway, const std::string& newGateway);
    void RunOptimization();
    void ApplyOptimizationPlan(const OptimizationPlan& plan);
    bool IsIPCoveredByExistingRoute(const std::string& ip);
    uint32_t IPToUInt(const std::string& ip);
    uint32_t CreateMask(int prefixLength);
    void NotifyUIRouteCountChanged();

    // Preload
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