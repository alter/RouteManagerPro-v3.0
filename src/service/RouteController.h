// src/service/RouteController.h
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include "../common/Models.h"

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

private:
    ServiceConfig config;
    std::unordered_map<std::string, std::unique_ptr<RouteInfo>> routes;
    mutable std::mutex routesMutex;
    std::atomic<bool> running;
    std::thread verifyThread;
    std::thread persistThread;

    // Persistence optimization
    std::atomic<bool> routesDirty{ false };
    std::chrono::steady_clock::time_point lastSaveTime;
    static constexpr auto SAVE_INTERVAL = std::chrono::minutes(10);

    bool AddSystemRoute(const std::string& ip);
    bool AddSystemRouteWithMask(const std::string& ip, int prefixLength);
    bool AddSystemRouteOldAPI(const std::string& ip);
    bool AddSystemRouteOldAPIWithMask(const std::string& ip, int prefixLength);
    bool RemoveSystemRoute(const std::string& ip);
    bool RemoveSystemRouteWithMask(const std::string& ip, int prefixLength);
    void VerifyRoutesThreadFunc();
    void PersistenceThreadFunc();
    void SaveRoutesToDisk();
    void SaveRoutesToDiskAsync();
    void LoadRoutesFromDisk();
    bool IsGatewayReachable();

    struct AIServiceRange {
        std::string service;
        std::vector<std::string> ranges;
    };

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