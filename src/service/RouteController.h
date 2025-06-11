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
    bool RemoveRoute(const std::string& ip);
    void CleanupAllRoutes();
    void CleanupOldRoutes();
    size_t GetRouteCount() const;
    std::vector<RouteInfo> GetActiveRoutes() const;
    void PreloadAIRoutes();
    bool RouteExists(const std::string& ip) const;

private:
    ServiceConfig config;
    std::unordered_map<std::string, std::unique_ptr<RouteInfo>> routes;
    mutable std::mutex routesMutex;
    std::atomic<bool> running;
    std::thread verifyThread;
    std::chrono::steady_clock::time_point lastSaveTime;

    bool AddSystemRoute(const std::string& ip);
    bool AddSystemRouteOldAPI(const std::string& ip);
    bool RemoveSystemRoute(const std::string& ip);
    void VerifyRoutesThreadFunc();
    void SaveRoutesToDisk();
    void LoadRoutesFromDisk();
    bool IsGatewayReachable();

    struct AIServiceRange {
        std::string service;
        std::vector<std::string> ranges;
    };

    std::vector<AIServiceRange> GetAIServiceRanges();
    std::pair<int, int> AddCIDRRoutes(const std::string& cidr, const std::string& service);
};