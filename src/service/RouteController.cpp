// src/service/RouteController.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "RouteController.h"
#include "RouteOptimizer.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <format>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <json/json.h>
#include <condition_variable>
#include <unordered_set>
#include <unordered_map>
#include <ranges>
#include <algorithm>
#include <bit>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

RouteController::RouteController(const ServiceConfig& cfg) : config(cfg), running(true),
lastSaveTime(std::chrono::steady_clock::now()), cachedInterfaceIndex(0),
lastOptimizationTime(std::chrono::steady_clock::now()) {
    LoadRoutesFromDisk();

    OptimizerConfig optConfig;
    optConfig.min_hosts_to_aggregate = config.optimizerSettings.minHostsToAggregate;
    optConfig.waste_thresholds = config.optimizerSettings.wasteThresholds;
    optimizer = std::make_unique<RouteOptimizer>(optConfig);

    verifyThread = std::jthread([this](std::stop_token token) { VerifyRoutesThreadFunc(token); });
    persistThread = std::jthread([this](std::stop_token token) { PersistenceThreadFunc(token); });
    optimizationThread = std::jthread([this](std::stop_token token) { OptimizationThreadFunc(token); });

    if (config.aiPreloadEnabled) {
        PreloadAIRoutes();
    }
}

RouteController::~RouteController() {
    Logger::Instance().Info("RouteController destructor - starting shutdown");

    running = false;
    optimizationCV.notify_all();

    // std::jthread автоматически вызовет request_stop() и join()

    if (routesDirty.load()) {
        Logger::Instance().Info("RouteController shutdown: Saving routes to disk");
        SaveRoutesToDisk();
    }

    Logger::Instance().Info("RouteController destructor - completed");
}

void RouteController::OptimizationThreadFunc(std::stop_token stopToken) {
    Logger::Instance().Info("RouteController optimization thread started");

    try {
        while (!stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown) {
            std::unique_lock<std::mutex> lock(optimizationMutex);

            auto waitUntil = std::chrono::steady_clock::now() + std::chrono::hours(1);
            optimizationCV.wait_until(lock, waitUntil, [&stopToken, this] {
                return stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown;
                });

            if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }

            lock.unlock();
            RunOptimization();
            lastOptimizationTime = std::chrono::steady_clock::now();
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error(std::format("OptimizationThreadFunc exception: {}", e.what()));
    }

    Logger::Instance().Info("RouteController optimization thread exiting");
}

std::vector<SystemRoute> RouteController::GetSystemRoutesForGateway() {
    std::vector<SystemRoute> systemRoutes;
    return GetSystemRoutesOldAPI();
}

std::vector<SystemRoute> RouteController::GetSystemRoutesOldAPI() {
    std::vector<SystemRoute> systemRoutes;

    PMIB_IPFORWARDTABLE pIpForwardTable = nullptr;
    DWORD dwSize = 0;

    if (GetIpForwardTable(pIpForwardTable, &dwSize, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
        pIpForwardTable = (PMIB_IPFORWARDTABLE)malloc(dwSize);

        if (pIpForwardTable && GetIpForwardTable(pIpForwardTable, &dwSize, FALSE) == NO_ERROR) {
            DWORD targetGateway = inet_addr(config.gatewayIp.c_str());

            for (DWORD i = 0; i < pIpForwardTable->dwNumEntries; i++) {
                const auto& row = pIpForwardTable->table[i];

                if (row.dwForwardNextHop == targetGateway) {
                    SystemRoute route;
                    route.address = ntohl(row.dwForwardDest);
                    route.mask = ntohl(row.dwForwardMask);
                    route.prefixLength = CountBits(route.mask);

                    struct in_addr addr;
                    addr.s_addr = row.dwForwardDest;
                    char ipStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &addr, ipStr, INET_ADDRSTRLEN);
                    route.ipString = ipStr;

                    systemRoutes.push_back(route);

                    if (systemRoutes.size() <= 5 || route.prefixLength < 32) {
                        Logger::Instance().Debug(std::format("Route: {}/{} addr={} mask={}",
                            route.ipString, route.prefixLength, route.address, route.mask));
                    }
                }
            }
        }

        if (pIpForwardTable) {
            free(pIpForwardTable);
        }
    }

    Logger::Instance().Info(std::format("Found {} routes for gateway {}",
        systemRoutes.size(), config.gatewayIp));

    std::unordered_map<int, int> prefixCounts;
    for (const auto& route : systemRoutes) {
        prefixCounts[route.prefixLength]++;
    }

    std::string distribution = "Route distribution by prefix: ";
    for (const auto& [prefix, count] : prefixCounts) {
        distribution += std::format("/{}={} ", prefix, count);
    }
    Logger::Instance().Info(distribution);

    return systemRoutes;
}

int RouteController::CountBits(uint32_t mask) {
    return std::popcount(mask);
}

void RouteController::RunOptimization() {
    Logger::Instance().Info("=== Starting Route Optimization (Deep Algorithm) ===");

    auto systemRoutes = GetSystemRoutesForGateway();
    Logger::Instance().Info(std::format("Found {} total routes in system for gateway {}",
        systemRoutes.size(), config.gatewayIp));

    std::vector<HostRoute> allRoutesForOptimization;
    std::vector<SystemRoute> largeAggregatedRoutes;

    for (const auto& route : systemRoutes) {
        if (route.prefixLength < 24) {
            largeAggregatedRoutes.push_back(route);
        }
        else {
            HostRoute hr;
            hr.ip = route.ipString;
            hr.ipNum = route.address;
            hr.prefixLength = route.prefixLength;

            std::string routeKey = std::format("{}/{}", route.ipString, route.prefixLength);
            auto it = routes.find(routeKey);
            if (it != routes.end()) {
                hr.processName = it->second->processName;
            }
            else {
                hr.processName = "Unknown";
            }

            allRoutesForOptimization.push_back(hr);
        }
    }

    Logger::Instance().Info(std::format("Prepared for optimization: {} routes (/24 and smaller), {} large aggregates kept",
        allRoutesForOptimization.size(), largeAggregatedRoutes.size()));

    std::unordered_map<uint32_t, std::vector<HostRoute>> routesByNetwork;
    for (const auto& route : allRoutesForOptimization) {
        uint32_t network = route.ipNum & 0xFFFFFF00;
        routesByNetwork[network].push_back(route);
    }

    Logger::Instance().Info(std::format("Routes are distributed across {} /24 networks",
        routesByNetwork.size()));

    std::vector<std::pair<uint32_t, size_t>> networkSizes;
    for (const auto& [network, routes] : routesByNetwork) {
        networkSizes.emplace_back(network, routes.size());
    }

    std::ranges::sort(networkSizes, [](const auto& a, const auto& b) {
        return a.second > b.second;
        });

    size_t maxNetworks = (std::min)(size_t(5), networkSizes.size());

    for (size_t i = 0; i < maxNetworks; i++) {
        uint32_t network = networkSizes[i].first;
        size_t count = networkSizes[i].second;
        struct in_addr addr;
        addr.s_addr = htonl(network);
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ipStr, INET_ADDRSTRLEN);
        Logger::Instance().Info(std::format("Network {}/24 has {} routes", ipStr, count));
    }

    std::vector<HostRoute> uncoveredRoutes;
    int coveredCount = 0;

    for (const auto& route : allRoutesForOptimization) {
        bool isCovered = std::ranges::any_of(largeAggregatedRoutes, [&](const auto& aggRoute) {
            uint32_t routeAddr = route.ipNum;
            uint32_t aggAddr = aggRoute.address;
            uint32_t mask = aggRoute.mask;
            return (routeAddr & mask) == (aggAddr & mask);
            });

        if (!isCovered) {
            uncoveredRoutes.push_back(route);
        }
        else {
            coveredCount++;
        }
    }

    Logger::Instance().Info(std::format("Filtered routes: {} already covered by large aggregates, {} routes need optimization",
        coveredCount, uncoveredRoutes.size()));

    auto plan = optimizer->OptimizeRoutes(uncoveredRoutes);

    if (plan.routesBefore > 0) {
        Logger::Instance().Info("Optimization Results:");
        Logger::Instance().Info(std::format("  Routes before: {}", plan.routesBefore));
        Logger::Instance().Info(std::format("  Routes after: {}", plan.routesAfter));
        Logger::Instance().Info(std::format("  Compression: {:.1f}%", plan.compressionRatio * 100));
        Logger::Instance().Info(std::format("  Savings: {} routes", plan.routesBefore - plan.routesAfter));

        int adds = 0, removes = 0;
        for (const auto& change : plan.changes) {
            if (change.type == OptimizationPlan::RouteChange::ADD) {
                adds++;
                Logger::Instance().Debug(std::format("  + Add: {}/{} ({})",
                    change.ip, change.prefixLength, change.reason));
            }
            else {
                removes++;
            }
        }
        Logger::Instance().Info(std::format("  Changes: {} additions, {} removals", adds, removes));
    }
    else {
        Logger::Instance().Info("No routes to optimize");
    }

    if (!plan.changes.empty()) {
        ApplyOptimizationPlan(plan);
    }

    Logger::Instance().Info("=== Route Optimization Completed ===");
}

void RouteController::RemoveRedundantSystemRoutes(
    const std::vector<HostRoute>& allHostRoutes,
    const std::vector<SystemRoute>& aggregatedRoutes) {

    Logger::Instance().Info("Removing redundant system routes");

    int removedCount = 0;
    int failedCount = 0;

    for (const auto& hostRoute : allHostRoutes) {
        bool isCovered = std::ranges::any_of(aggregatedRoutes, [&](const auto& aggRoute) {
            return (hostRoute.ipNum & aggRoute.mask) == (aggRoute.address & aggRoute.mask);
            });

        if (isCovered) {
            if (RemoveSystemRouteWithMask(hostRoute.ip, 32, config.gatewayIp)) {
                removedCount++;

                std::lock_guard<std::mutex> lock(routesMutex);
                std::string routeKey = std::format("{}/32", hostRoute.ip);
                routes.erase(routeKey);
                routesDirty = true;
            }
            else {
                failedCount++;
            }
        }
    }

    Logger::Instance().Info(std::format("Removed {} redundant routes, {} failed",
        removedCount, failedCount));

    if (removedCount > 0) {
        NotifyUIRouteCountChanged();
    }
}

void RouteController::SyncWithSystemTable() {
    Logger::Instance().Info("Syncing with system routing table");

    auto systemRoutes = GetSystemRoutesForGateway();

    std::unordered_set<std::string> systemRouteKeys;
    for (const auto& route : systemRoutes) {
        std::string key = std::format("{}/{}", route.ipString, route.prefixLength);
        systemRouteKeys.insert(key);
    }

    std::vector<std::string> toRemove;
    {
        std::lock_guard<std::mutex> lock(routesMutex);

        for (const auto& [routeKey, routeInfo] : routes) {
            if (!systemRouteKeys.contains(routeKey)) {
                Logger::Instance().Warning(std::format("Route {} exists in state but not in system, marking for removal",
                    routeKey));
                toRemove.push_back(routeKey);
            }
        }

        for (const auto& key : toRemove) {
            routes.erase(key);
        }
    }

    int addedCount = 0;
    for (const auto& sysRoute : systemRoutes) {
        std::string key = std::format("{}/{}", sysRoute.ipString, sysRoute.prefixLength);

        std::lock_guard<std::mutex> lock(routesMutex);
        if (!routes.contains(key)) {
            auto route = std::make_unique<RouteInfo>();
            route->ip = sysRoute.ipString;
            route->prefixLength = sysRoute.prefixLength;
            route->refCount = 1;
            route->processName = "System";
            route->createdAt = std::chrono::system_clock::now();

            routes[key] = std::move(route);
            addedCount++;
        }
    }

    Logger::Instance().Info(std::format("Sync completed: removed {} orphaned routes, added {} system routes",
        toRemove.size(), addedCount));

    if (!toRemove.empty() || addedCount > 0) {
        routesDirty = true;
    }
}

void RouteController::PerformFullCleanup() {
    Logger::Instance().Info("Starting smart route cleanup");

    SyncWithSystemTable();
    RunOptimization();
    SaveRoutesToDisk();

    Logger::Instance().Info("Smart cleanup completed");
}

void RouteController::CleanupRedundantRoutes() {
    Logger::Instance().Info("CleanupRedundantRoutes - Starting cleanup of redundant routes only");

    auto systemRoutes = GetSystemRoutesForGateway();

    std::vector<HostRoute> hostRoutes;
    std::vector<SystemRoute> aggregatedRoutes;

    for (const auto& route : systemRoutes) {
        if (route.prefixLength == 32) {
            HostRoute hr;
            hr.ip = route.ipString;
            hr.ipNum = route.address;
            hr.processName = "System";
            hostRoutes.push_back(hr);
        }
        else {
            aggregatedRoutes.push_back(route);
        }
    }

    RemoveRedundantSystemRoutes(hostRoutes, aggregatedRoutes);

    Logger::Instance().Info("CleanupRedundantRoutes - Completed");
}

void RouteController::ApplyOptimizationPlan(const OptimizationPlan& plan) {
    std::vector<std::pair<std::string, int>> toRemove;
    std::vector<std::pair<std::string, int>> toAdd;
    std::vector<std::pair<std::string, int>> addedRoutes;

    for (const auto& change : plan.changes) {
        if (change.type == OptimizationPlan::RouteChange::ADD) {
            toAdd.emplace_back(change.ip, change.prefixLength);
        }
        else {
            toRemove.emplace_back(change.ip, change.prefixLength);
        }
    }

    bool addFailed = false;
    for (const auto& [ip, prefix] : toAdd) {
        if (AddSystemRouteWithMask(ip, prefix)) {
            addedRoutes.emplace_back(ip, prefix);
        }
        else {
            Logger::Instance().Error(std::format("Failed to add aggregated route: {}/{}", ip, prefix));
            addFailed = true;
            break;
        }
    }

    if (addFailed) {
        Logger::Instance().Warning("Rolling back optimization due to add failure");
        for (const auto& [ip, prefix] : addedRoutes) {
            RemoveSystemRouteWithMask(ip, prefix, config.gatewayIp);
        }
        return;
    }

    for (const auto& [ip, prefix] : toRemove) {
        if (!RemoveSystemRouteWithMask(ip, prefix, config.gatewayIp)) {
            Logger::Instance().Warning(std::format("Failed to remove host route: {}/{}", ip, prefix));
        }
    }

    {
        std::lock_guard<std::mutex> lock(routesMutex);

        for (const auto& change : plan.changes) {
            if (change.type == OptimizationPlan::RouteChange::ADD) {
                std::string routeKey = std::format("{}/{}", change.ip, change.prefixLength);
                auto routeInfo = std::make_unique<RouteInfo>(change.ip, "Optimized");
                routeInfo->prefixLength = change.prefixLength;
                routes[routeKey] = std::move(routeInfo);
            }
        }

        for (const auto& change : plan.changes) {
            if (change.type == OptimizationPlan::RouteChange::REMOVE) {
                std::string routeKey = std::format("{}/{}", change.ip, change.prefixLength);
                routes.erase(routeKey);
            }
        }

        routesDirty = true;
    }

    NotifyUIRouteCountChanged();
}

void RouteController::NotifyUIRouteCountChanged() {
    HWND uiWindow = FindWindow(L"RouteManagerProWindow", nullptr);
    if (uiWindow) {
        PostMessage(uiWindow, WM_USER + 101, 0, 0);
    }
}

void RouteController::RunOptimizationManual() {
    Logger::Instance().Info("Manual optimization requested");
    RunOptimization();
    lastOptimizationTime = std::chrono::steady_clock::now();
}

void RouteController::InvalidateInterfaceCache() {
    std::lock_guard<std::mutex> lock(interfaceCacheMutex);
    cachedInterfaceIndex = 0;
    Logger::Instance().Info("Interface cache invalidated");
}

void RouteController::UpdateConfig(const ServiceConfig& newConfig) {
    std::lock_guard<std::mutex> lock(routesMutex);

    bool gatewayChanged = (config.gatewayIp != newConfig.gatewayIp);
    bool metricChanged = (config.metric != newConfig.metric);

    std::string oldGateway = config.gatewayIp;
    config = newConfig;

    if (optimizer) {
        OptimizerConfig optConfig;
        optConfig.min_hosts_to_aggregate = config.optimizerSettings.minHostsToAggregate;
        optConfig.waste_thresholds = config.optimizerSettings.wasteThresholds;
        optimizer->UpdateConfig(optConfig);
    }

    if (gatewayChanged) {
        Logger::Instance().Info(std::format("Gateway changed from {} to {}", oldGateway, config.gatewayIp));
        InvalidateInterfaceCache();
        MigrateExistingRoutes(oldGateway, config.gatewayIp);
    }
    else if (metricChanged) {
        Logger::Instance().Info("Metric changed, updating all routes");
        MigrateExistingRoutes(config.gatewayIp, config.gatewayIp);
    }
}

void RouteController::MigrateExistingRoutes(const std::string& oldGateway, const std::string& newGateway) {
    std::vector<std::pair<std::string, int>> routesToMigrate;
    for (const auto& [key, routeInfo] : routes) {
        routesToMigrate.emplace_back(routeInfo->ip, routeInfo->prefixLength);
    }

    Logger::Instance().Info(std::format("Migrating {} routes from gateway {} to {}",
        routesToMigrate.size(), oldGateway, newGateway));

    int successCount = 0;
    int failCount = 0;

    for (const auto& [ip, prefix] : routesToMigrate) {
        RemoveSystemRouteWithMask(ip, prefix, oldGateway);

        if (AddSystemRouteWithMask(ip, prefix)) {
            successCount++;
        }
        else {
            failCount++;
            Logger::Instance().Error(std::format("Failed to migrate route: {}/{}", ip, prefix));
        }
    }

    Logger::Instance().Info(std::format("Migration complete. Success: {}, Failed: {}",
        successCount, failCount));
}

void RouteController::PersistenceThreadFunc(std::stop_token stopToken) {
    Logger::Instance().Info("RouteController persistence thread started");

    try {
        while (!stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown) {
            for (int i = 0; i < 60 && !stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }

            auto now = std::chrono::steady_clock::now();
            auto timeSinceLastSave = now - lastSaveTime;

            if (routesDirty.load() && timeSinceLastSave >= SAVE_INTERVAL) {
                Logger::Instance().Info("Periodic save of routes (dirty flag set)");
                SaveRoutesToDiskAsync();
            }
        }

        if (routesDirty.load()) {
            Logger::Instance().Info("Persistence thread: Final save of routes");
            SaveRoutesToDisk();
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error(std::format("PersistenceThreadFunc exception: {}", e.what()));
    }

    Logger::Instance().Info("RouteController persistence thread exiting");
}

void RouteController::SaveRoutesToDiskAsync() {
    std::vector<std::pair<std::string, RouteInfo>> snapshot;
    {
        std::lock_guard<std::mutex> lock(routesMutex);
        for (const auto& [key, routeInfo] : routes) {
            snapshot.emplace_back(key, *routeInfo);
        }
    }

    std::ofstream file(Constants::STATE_FILE + ".tmp");
    if (!file.is_open()) {
        Logger::Instance().Error("Failed to open state file for writing");
        return;
    }

    file << "version=3\n";

    auto now = std::chrono::system_clock::now();
    auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    file << std::format("timestamp={}\n", nowSeconds);
    file << std::format("gateway={}\n", config.gatewayIp);

    for (const auto& [routeKey, route] : snapshot) {
        auto createdSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            route.createdAt.time_since_epoch()).count();

        file << std::format("route={},{},{},{},{}\n",
            route.ip, route.processName, createdSeconds, route.prefixLength, config.gatewayIp);
    }

    file.close();

    if (std::filesystem::exists(Constants::STATE_FILE + ".tmp")) {
        std::filesystem::rename(Constants::STATE_FILE + ".tmp", Constants::STATE_FILE);
    }

    routesDirty = false;
    lastSaveTime = std::chrono::steady_clock::now();

    Logger::Instance().Info(std::format("Routes saved to disk: {} routes", snapshot.size()));
}

bool RouteController::AddRoute(const std::string& ip, const std::string& processName) {
    return AddRouteWithMask(ip, 32, processName);
}

bool RouteController::AddRouteWithMask(const std::string& ip, int prefixLength, const std::string& processName) {
    if (!Utils::IsValidIPv4(ip)) return false;

    if (Utils::IsPrivateIP(ip)) {
        Logger::Instance().Debug(std::format("Skipping private IP: {}", ip));
        return false;
    }

    std::lock_guard<std::mutex> lock(routesMutex);

    if (IsIPCoveredByExistingRoute(ip)) {
        Logger::Instance().Info(std::format("IP {} is already covered by an aggregated route, skipping addition", ip));
        return true;
    }

    std::string routeKey = std::format("{}/{}", ip, prefixLength);

    if (routes.size() >= Constants::MAX_ROUTES) {
        CleanupOldRoutes();
    }

    auto it = routes.find(routeKey);
    if (it != routes.end()) {
        it->second->refCount++;
        Logger::Instance().Info(std::format("Route already exists, incrementing ref count: {} (refs: {})",
            routeKey, it->second->refCount.load()));
        return true;
    }

    if (AddSystemRouteWithMask(ip, prefixLength)) {
        auto routeInfo = std::make_unique<RouteInfo>(ip, processName);
        routeInfo->prefixLength = prefixLength;
        routes[routeKey] = std::move(routeInfo);
        Logger::Instance().Info(std::format("Added new route: {} for process: {}", routeKey, processName));
        routesDirty = true;

        NotifyUIRouteCountChanged();
        return true;
    }

    return false;
}

bool RouteController::RemoveRoute(const std::string& ip) {
    return RemoveRouteWithMask(ip, 32);
}

bool RouteController::RemoveRouteWithMask(const std::string& ip, int prefixLength) {
    std::lock_guard<std::mutex> lock(routesMutex);

    std::string routeKey = std::format("{}/{}", ip, prefixLength);
    auto it = routes.find(routeKey);
    if (it == routes.end()) return false;

    if (--it->second->refCount <= 0) {
        if (RemoveSystemRouteWithMask(ip, prefixLength, config.gatewayIp)) {
            Logger::Instance().Info(std::format("Removed route: {}", routeKey));
            routes.erase(it);
            routesDirty = true;

            NotifyUIRouteCountChanged();
            return true;
        }
    }

    return true;
}

void RouteController::CleanupAllRoutes() {
    Logger::Instance().Info("CleanupAllRoutes - Starting cleanup of all routes");

    std::vector<std::pair<std::string, int>> routesToDelete;
    bool hadPreloadRoutes = false;
    {
        std::lock_guard<std::mutex> lock(routesMutex);
        if (routes.empty()) {
            Logger::Instance().Info("CleanupAllRoutes - No routes to clean");
            return;
        }

        for (const auto& [routeKey, routeInfo] : routes) {
            routesToDelete.emplace_back(routeInfo->ip, routeInfo->prefixLength);
            if (routeInfo->processName.starts_with("Preload-")) {
                hadPreloadRoutes = true;
            }
        }

        routes.clear();
        routesDirty = true;
    }

    int successCount = 0;
    int failCount = 0;

    for (const auto& [ip, prefixLength] : routesToDelete) {
        Logger::Instance().Info(std::format("Removing Windows route for: {}/{}", ip, prefixLength));
        if (RemoveSystemRouteWithMask(ip, prefixLength, config.gatewayIp)) {
            successCount++;
        }
        else {
            Logger::Instance().Error(std::format("Failed to remove Windows route for: {}/{}", ip, prefixLength));
            failCount++;
        }
    }

    if (hadPreloadRoutes) {
        config.aiPreloadEnabled = false;
        Logger::Instance().Info("CleanupAllRoutes - Disabled AI preload since preload routes were removed");
    }

    SaveRoutesToDisk();
    NotifyUIRouteCountChanged();

    Logger::Instance().Info(std::format("CleanupAllRoutes - Completed. Removed: {}, Failed: {}",
        successCount, failCount));
}

void RouteController::CleanupOldRoutes() {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(Constants::ROUTE_CLEANUP_HOURS);

    bool anyRemoved = false;
    for (auto it = routes.begin(); it != routes.end();) {
        if (it->second->createdAt < cutoff) {
            RemoveSystemRouteWithMask(it->second->ip, it->second->prefixLength, config.gatewayIp);
            it = routes.erase(it);
            anyRemoved = true;
        }
        else {
            ++it;
        }
    }

    if (anyRemoved) {
        routesDirty = true;
        NotifyUIRouteCountChanged();
    }
}

size_t RouteController::GetRouteCount() const {
    std::lock_guard<std::mutex> lock(routesMutex);
    return routes.size();
}

std::vector<RouteInfo> RouteController::GetActiveRoutes() const {
    std::lock_guard<std::mutex> lock(routesMutex);
    std::vector<RouteInfo> result;

    for (const auto& [key, routeInfo] : routes) {
        result.push_back(*routeInfo);
    }

    std::ranges::sort(result, [](const RouteInfo& a, const RouteInfo& b) {
        return a.createdAt > b.createdAt;
        });

    return result;
}

bool RouteController::IsIPCoveredByExistingRoute(const std::string& ip) {
    uint32_t ipAddr = IPToUInt(ip);

    for (const auto& [routeKey, routeInfo] : routes) {
        if (routeInfo->prefixLength >= 32) continue;

        uint32_t routeAddr = IPToUInt(routeInfo->ip);
        uint32_t mask = CreateMask(routeInfo->prefixLength);

        if ((ipAddr & mask) == (routeAddr & mask)) {
            Logger::Instance().Debug(std::format("IP {} is covered by route {}", ip, routeKey));
            return true;
        }
    }

    return false;
}

uint32_t RouteController::IPToUInt(const std::string& ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) == 1) {
        return ntohl(addr.s_addr);
    }
    return 0;
}

constexpr uint32_t RouteController::CreateMask(int prefixLength) {
    if (prefixLength <= 0) return 0;
    if (prefixLength >= 32) return 0xFFFFFFFF;
    return ~((1u << (32 - prefixLength)) - 1);
}

bool RouteController::AddSystemRoute(const std::string& ip) {
    return AddSystemRouteWithMask(ip, 32);
}

bool RouteController::AddSystemRouteWithMask(const std::string& ip, int prefixLength) {
    MIB_IPFORWARD_ROW2 route;
    InitializeIpForwardEntry(&route);

    SOCKADDR_INET destAddr = { 0 };
    SOCKADDR_INET nextHop = { 0 };

    destAddr.si_family = AF_INET;
    if (inet_pton(AF_INET, ip.c_str(), &destAddr.Ipv4.sin_addr) != 1) {
        Logger::Instance().Error(std::format("Invalid destination IP: {}", ip));
        return false;
    }

    nextHop.si_family = AF_INET;
    if (inet_pton(AF_INET, config.gatewayIp.c_str(), &nextHop.Ipv4.sin_addr) != 1) {
        Logger::Instance().Error(std::format("Invalid gateway IP: {}", config.gatewayIp));
        return false;
    }

    NET_IFINDEX bestInterface = 0;
    {
        std::lock_guard<std::mutex> lock(interfaceCacheMutex);
        if (cachedInterfaceIndex != 0) {
            bestInterface = cachedInterfaceIndex;
        }
    }

    if (bestInterface == 0) {
        DWORD result = GetBestInterface(nextHop.Ipv4.sin_addr.s_addr, &bestInterface);
        if (result != NO_ERROR) {
            Logger::Instance().Error(std::format("GetBestInterface failed: {}", result));
            return false;
        }
        std::lock_guard<std::mutex> lock(interfaceCacheMutex);
        cachedInterfaceIndex = bestInterface;
    }

    route.InterfaceIndex = bestInterface;
    route.DestinationPrefix.Prefix = destAddr;
    route.DestinationPrefix.PrefixLength = prefixLength;
    route.NextHop = nextHop;
    route.Protocol = MIB_IPPROTO_NETMGMT;
    route.Metric = config.metric;

    Logger::Instance().Debug(std::format("Adding route via CreateIpForwardEntry2: {}/{} -> {} (interface: {})",
        ip, prefixLength, config.gatewayIp, bestInterface));

    DWORD result = CreateIpForwardEntry2(&route);

    if (result == NO_ERROR) {
        Logger::Instance().Info(std::format("Successfully added route: {}/{} -> {}",
            ip, prefixLength, config.gatewayIp));
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        Logger::Instance().Debug(std::format("Route already exists: {}/{}", ip, prefixLength));
        return true;
    }
    else {
        Logger::Instance().Error(std::format("CreateIpForwardEntry2 failed: {}", result));

        if (result == ERROR_NOT_FOUND || result == ERROR_INVALID_FUNCTION) {
            return AddSystemRouteOldAPIWithMask(ip, prefixLength);
        }

        return false;
    }
}

bool RouteController::AddSystemRouteOldAPI(const std::string& ip) {
    return AddSystemRouteOldAPIWithMask(ip, 32);
}

bool RouteController::AddSystemRouteOldAPIWithMask(const std::string& ip, int prefixLength) {
    Logger::Instance().Info("Falling back to old API for compatibility");

    MIB_IPFORWARDROW route = { 0 };

    route.dwForwardDest = inet_addr(ip.c_str());
    if (route.dwForwardDest == INADDR_NONE) {
        Logger::Instance().Error(std::format("Invalid IP address: {}", ip));
        return false;
    }

    DWORD mask = prefixLength == 0 ? 0 : (0xFFFFFFFF << (32 - prefixLength));
    route.dwForwardMask = htonl(mask);

    route.dwForwardPolicy = 0;
    route.dwForwardNextHop = inet_addr(config.gatewayIp.c_str());

    if (route.dwForwardNextHop == INADDR_NONE) {
        Logger::Instance().Error(std::format("Invalid gateway IP: {}", config.gatewayIp));
        return false;
    }

    DWORD bestInterface = 0;
    DWORD result = GetBestInterface(route.dwForwardNextHop, &bestInterface);
    if (result != NO_ERROR) {
        Logger::Instance().Error(std::format("GetBestInterface failed: {}", result));
        return false;
    }

    route.dwForwardIfIndex = bestInterface;

    MIB_IPINTERFACE_ROW iface;
    InitializeIpInterfaceEntry(&iface);
    iface.Family = AF_INET;
    iface.InterfaceIndex = bestInterface;

    ULONG minMetric = config.metric;
    result = GetIpInterfaceEntry(&iface);
    if (result == NO_ERROR) {
        minMetric = iface.Metric + config.metric;
        Logger::Instance().Info(std::format("Interface metric: {}, using route metric: {}",
            iface.Metric, minMetric));
    }
    else {
        Logger::Instance().Warning(std::format("GetIpInterfaceEntry failed: {}, using default metric", result));
    }

    route.dwForwardType = 4;
    route.dwForwardProto = 3;
    route.dwForwardAge = 0;
    route.dwForwardNextHopAS = 0;
    route.dwForwardMetric1 = minMetric;
    route.dwForwardMetric2 = 0xFFFFFFFF;
    route.dwForwardMetric3 = 0xFFFFFFFF;
    route.dwForwardMetric4 = 0xFFFFFFFF;
    route.dwForwardMetric5 = 0xFFFFFFFF;

    result = CreateIpForwardEntry(&route);

    if (result == NO_ERROR) {
        Logger::Instance().Info(std::format("Successfully added route via old API: {}/{}", ip, prefixLength));
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        Logger::Instance().Debug(std::format("Route already exists: {}/{}", ip, prefixLength));
        return true;
    }
    else {
        Logger::Instance().Error(std::format("CreateIpForwardEntry failed: {}", result));
        return false;
    }
}

bool RouteController::RemoveSystemRoute(const std::string& ip, const std::string& gatewayIp) {
    return RemoveSystemRouteWithMask(ip, 32, gatewayIp);
}

bool RouteController::RemoveSystemRouteWithMask(const std::string& ip, int prefixLength, const std::string& gatewayIp) {
    MIB_IPFORWARDROW route;
    ZeroMemory(&route, sizeof(MIB_IPFORWARDROW));

    route.dwForwardDest = inet_addr(ip.c_str());
    if (route.dwForwardDest == INADDR_NONE) {
        Logger::Instance().Error(std::format("Invalid IP address: {}", ip));
        return false;
    }

    DWORD mask = prefixLength == 0 ? 0 : (0xFFFFFFFF << (32 - prefixLength));
    route.dwForwardMask = htonl(mask);

    route.dwForwardNextHop = inet_addr(gatewayIp.c_str());

    ULONG bestInterface;
    if (GetBestInterface(route.dwForwardNextHop, &bestInterface) == NO_ERROR) {
        route.dwForwardIfIndex = bestInterface;
    }

    DWORD result = DeleteIpForwardEntry(&route);

    if (result == NO_ERROR) {
        Logger::Instance().Debug(std::format("Successfully removed route via API: {}/{}", ip, prefixLength));
        return true;
    }
    else if (result == ERROR_NOT_FOUND) {
        Logger::Instance().Debug(std::format("Route not found: {}/{}", ip, prefixLength));
        return true;
    }
    else {
        Logger::Instance().Error(std::format("Failed to remove route via API: {}/{}, error: {}",
            ip, prefixLength, result));
        return false;
    }
}

void RouteController::VerifyRoutesThreadFunc(std::stop_token stopToken) {
    Logger::Instance().Info("RouteController verify thread started");

    try {
        while (!stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown) {
            for (int i = 0; i < Constants::ROUTE_VERIFY_INTERVAL_SEC &&
                !stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }

            if (!IsGatewayReachable()) {
                InvalidateInterfaceCache();
                continue;
            }

            std::vector<std::pair<std::string, int>> routesToVerify;
            {
                std::lock_guard<std::mutex> lock(routesMutex);
                if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
                    break;
                }

                for (const auto& [key, routeInfo] : routes) {
                    routesToVerify.emplace_back(routeInfo->ip, routeInfo->prefixLength);
                }
            }

            for (const auto& [ip, prefixLength] : routesToVerify) {
                if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
                    Logger::Instance().Info("Route verification interrupted by shutdown");
                    break;
                }

                AddSystemRouteWithMask(ip, prefixLength);
            }
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error(std::format("VerifyRoutesThreadFunc exception: {}", e.what()));
    }

    Logger::Instance().Info("RouteController verify thread exiting");
}

void RouteController::SaveRoutesToDisk() {
    std::lock_guard<std::mutex> lock(routesMutex);

    std::ofstream file(Constants::STATE_FILE + ".tmp");
    if (!file.is_open()) return;

    file << "version=3\n";

    auto now = std::chrono::system_clock::now();
    auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    file << std::format("timestamp={}\n", nowSeconds);
    file << std::format("gateway={}\n", config.gatewayIp);

    for (const auto& [key, routeInfo] : routes) {
        auto createdSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            routeInfo->createdAt.time_since_epoch()).count();

        file << std::format("route={},{},{},{},{}\n",
            routeInfo->ip, routeInfo->processName, createdSeconds,
            routeInfo->prefixLength, config.gatewayIp);
    }

    file.close();

    if (std::filesystem::exists(Constants::STATE_FILE + ".tmp")) {
        std::filesystem::rename(Constants::STATE_FILE + ".tmp", Constants::STATE_FILE);
    }

    routesDirty = false;
    lastSaveTime = std::chrono::steady_clock::now();
}

void RouteController::LoadRoutesFromDisk() {
    if (!Utils::FileExists(Constants::STATE_FILE)) return;

    std::ifstream file(Constants::STATE_FILE);
    if (!file.is_open()) return;

    std::string line;
    int loadedCount = 0;
    int skippedPreloadCount = 0;
    std::string savedGateway;

    while (std::getline(file, line)) {
        if (line.starts_with("gateway=")) {
            savedGateway = line.substr(8);
        }
        else if (line.starts_with("route=")) {
            std::string routeData = line.substr(6);
            auto parts = Utils::SplitString(routeData, ',');
            if (parts.size() >= 2) {
                std::string ip = parts[0];
                std::string process = parts[1];
                int prefixLength = 32;

                if (process.starts_with("Preload-")) {
                    skippedPreloadCount++;
                    continue;
                }

                std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();

                if (parts.size() >= 3 && !parts[2].empty()) {
                    try {
                        int64_t timestamp = std::stoll(parts[2]);
                        if (timestamp > 0 && timestamp < 9999999999LL) {
                            createdAt = std::chrono::system_clock::time_point(
                                std::chrono::seconds(timestamp));
                        }
                    }
                    catch (...) {
                        Logger::Instance().Warning(std::format("Failed to parse timestamp for route: {}", ip));
                    }
                }

                if (parts.size() >= 4) {
                    try {
                        prefixLength = std::stoi(parts[3]);
                    }
                    catch (...) {
                        Logger::Instance().Warning(std::format("Failed to parse prefix length for route: {}", ip));
                    }
                }

                std::string routeGateway = config.gatewayIp;
                if (parts.size() >= 5) {
                    routeGateway = parts[4];
                }

                if (AddSystemRouteWithMask(ip, prefixLength)) {
                    std::string routeKey = std::format("{}/{}", ip, prefixLength);
                    auto routeInfo = std::make_unique<RouteInfo>(ip, process);
                    routeInfo->prefixLength = prefixLength;
                    routeInfo->createdAt = createdAt;
                    routes[routeKey] = std::move(routeInfo);
                    loadedCount++;
                }
            }
        }
    }

    if (!savedGateway.empty() && savedGateway != config.gatewayIp) {
        Logger::Instance().Warning(std::format("Gateway mismatch on startup. Saved: {}, Config: {}. Migrating routes.",
            savedGateway, config.gatewayIp));
        MigrateExistingRoutes(savedGateway, config.gatewayIp);
    }

    Logger::Instance().Info(std::format("LoadRoutesFromDisk - Loaded {} routes, skipped {} preload routes",
        loadedCount, skippedPreloadCount));

    routesDirty = false;
}

bool RouteController::IsGatewayReachable() {
    ULONG destAddr = inet_addr(config.gatewayIp.c_str());
    ULONG srcAddr = INADDR_ANY;
    ULONG bestIfIndex;

    if (GetBestInterface(destAddr, &bestIfIndex) != NO_ERROR) {
        return false;
    }

    return true;
}

void RouteController::PreloadAIRoutes() {
    Logger::Instance().Info("PreloadRoutes - Starting preload of IP ranges from config");

    auto services = LoadPreloadConfig();

    int totalRoutes = 0;
    for (const auto& service : services) {
        if (!service.enabled) {
            Logger::Instance().Info(std::format("Skipping disabled service: {}", service.name));
            continue;
        }

        Logger::Instance().Info(std::format("Processing {} ranges", service.name));
        for (const auto& range : service.ranges) {
            if (range.contains('/')) {
                if (AddCIDRRoute(range, service.name)) {
                    totalRoutes++;
                }
            }
            else {
                if (AddRoute(range, std::format("Preload-{}", service.name))) {
                    totalRoutes++;
                }
            }
        }
    }
    Logger::Instance().Info(std::format("PreloadRoutes - Completed, added {} routes", totalRoutes));
}

std::vector<RouteController::PreloadService> RouteController::LoadPreloadConfig() {
    std::vector<PreloadService> services;

    std::string configPath = std::format("{}\\preload_ips.json", Utils::GetCurrentDirectory());

    if (!Utils::FileExists(configPath)) {
        CreateDefaultPreloadConfig(configPath);
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        Logger::Instance().Error(std::format("Failed to open preload config: {}", configPath));
        return GetDefaultPreloadServices();
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;

    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        Logger::Instance().Error(std::format("Failed to parse preload config: {}", errors));
        return GetDefaultPreloadServices();
    }

    const Json::Value& servicesJson = root["services"];
    if (!servicesJson.isArray()) {
        Logger::Instance().Error("Invalid preload config format");
        return GetDefaultPreloadServices();
    }

    for (const auto& serviceJson : servicesJson) {
        PreloadService service;
        service.name = serviceJson.get("name", "").asString();
        service.enabled = serviceJson.get("enabled", true).asBool();

        const Json::Value& rangesJson = serviceJson["ranges"];
        if (rangesJson.isArray()) {
            for (const auto& range : rangesJson) {
                service.ranges.push_back(range.asString());
            }
        }

        if (!service.name.empty() && !service.ranges.empty()) {
            services.push_back(service);
        }
    }

    Logger::Instance().Info(std::format("Loaded {} services from preload config", services.size()));
    return services;
}

void RouteController::CreateDefaultPreloadConfig(const std::string& path) {
    std::string sourceFile = std::format("{}\\config\\preload_ips.json", Utils::GetCurrentDirectory());

    if (Utils::FileExists(sourceFile)) {
        std::ifstream src(sourceFile, std::ios::binary);
        if (src.is_open()) {
            std::ofstream dst(path, std::ios::binary);
            if (dst.is_open()) {
                dst << src.rdbuf();
                Logger::Instance().Info(std::format("Copied default preload config from: {}", sourceFile));
                return;
            }
        }
    }

    Logger::Instance().Warning(std::format("Could not copy default config from {}, using fallback", sourceFile));

    std::ofstream file(path);
    if (file.is_open()) {
        file << R"({
  "version": 1,
  "services": [
    {
      "name": "Discord",
      "enabled": true,
      "ranges": [
        "162.159.128.0/19"
      ]
    }
  ]
})";
        file.close();
        Logger::Instance().Info(std::format("Created minimal fallback preload config: {}", path));
    }
}

std::vector<RouteController::PreloadService> RouteController::GetDefaultPreloadServices() {
    return {
        {"Discord", true, { "162.159.128.0/19" }}
    };
}

bool RouteController::AddCIDRRoute(const std::string& cidr, const std::string& service) {
    size_t slashPos = cidr.find('/');
    if (slashPos == std::string::npos) return false;

    std::string baseIp = cidr.substr(0, slashPos);
    int prefixLen = std::stoi(cidr.substr(slashPos + 1));

    Logger::Instance().Info(std::format("Adding CIDR route: {} for {}", cidr, service));

    return AddRouteWithMask(baseIp, prefixLen, std::format("Preload-{}", service));
}