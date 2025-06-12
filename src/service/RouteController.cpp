// src/service/RouteController.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "RouteController.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <sstream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <json/json.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

RouteController::RouteController(const ServiceConfig& cfg) : config(cfg), running(true),
lastSaveTime(std::chrono::steady_clock::now()), cachedInterfaceIndex(0) {
    LoadRoutesFromDisk();
    verifyThread = std::thread(&RouteController::VerifyRoutesThreadFunc, this);
    persistThread = std::thread(&RouteController::PersistenceThreadFunc, this);
    if (config.aiPreloadEnabled) {
        PreloadAIRoutes();
    }
}

RouteController::~RouteController() {
    Logger::Instance().Info("RouteController destructor - starting shutdown");

    // Signal threads to stop
    running = false;

    // Wait for verify thread
    if (verifyThread.joinable()) {
        Logger::Instance().Info("Waiting for verify thread to stop...");
        try {
            verifyThread.join();
            Logger::Instance().Info("Verify thread joined successfully");
        }
        catch (const std::exception& e) {
            Logger::Instance().Error("Exception joining verify thread: " + std::string(e.what()));
        }
    }

    // Wait for persist thread
    if (persistThread.joinable()) {
        Logger::Instance().Info("Waiting for persist thread to stop...");
        try {
            persistThread.join();
            Logger::Instance().Info("Persist thread joined successfully");
        }
        catch (const std::exception& e) {
            Logger::Instance().Error("Exception joining persist thread: " + std::string(e.what()));
        }
    }

    // Final save on shutdown
    if (routesDirty.load()) {
        Logger::Instance().Info("RouteController shutdown: Saving routes to disk");
        SaveRoutesToDisk();
    }

    Logger::Instance().Info("RouteController destructor - completed");
}

void RouteController::InvalidateInterfaceCache() {
    std::lock_guard<std::mutex> lock(interfaceCacheMutex);
    cachedInterfaceIndex = 0;
    Logger::Instance().Info("Interface cache invalidated");
}

void RouteController::PersistenceThreadFunc() {
    Logger::Instance().Info("RouteController persistence thread started");

    try {
        while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
            // Check every minute if we need to save - but check shutdown every second
            for (int i = 0; i < 60 && running.load() && !ShutdownCoordinator::Instance().isShuttingDown; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }

            auto now = std::chrono::steady_clock::now();
            auto timeSinceLastSave = now - lastSaveTime;

            // Save if dirty and enough time has passed
            if (routesDirty.load() && timeSinceLastSave >= SAVE_INTERVAL) {
                Logger::Instance().Info("Periodic save of routes (dirty flag set)");
                SaveRoutesToDiskAsync();
            }
        }

        // Final save on thread exit
        if (routesDirty.load()) {
            Logger::Instance().Info("Persistence thread: Final save of routes");
            SaveRoutesToDisk();
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("PersistenceThreadFunc exception: " + std::string(e.what()));
    }

    // Log AFTER all work is done, right before thread exits
    Logger::Instance().Info("RouteController persistence thread exiting");
}

void RouteController::SaveRoutesToDiskAsync() {
    // Create a snapshot of routes under lock
    std::vector<std::pair<std::string, RouteInfo>> snapshot;
    {
        std::lock_guard<std::mutex> lock(routesMutex);
        for (const auto& [key, route] : routes) {
            snapshot.emplace_back(key, *route);
        }
    }

    // Save without holding the lock
    std::ofstream file(Constants::STATE_FILE + ".tmp");
    if (!file.is_open()) {
        Logger::Instance().Error("Failed to open state file for writing");
        return;
    }

    file << "version=2\n";

    auto now = std::chrono::system_clock::now();
    auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    file << "timestamp=" << nowSeconds << "\n";

    for (const auto& [routeKey, route] : snapshot) {
        auto createdSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            route.createdAt.time_since_epoch()).count();

        file << "route=" << route.ip << "," << route.processName << ","
            << createdSeconds << "," << route.prefixLength << "\n";
    }

    file.close();

    if (std::filesystem::exists(Constants::STATE_FILE + ".tmp")) {
        std::filesystem::rename(Constants::STATE_FILE + ".tmp", Constants::STATE_FILE);
    }

    routesDirty = false;
    lastSaveTime = std::chrono::steady_clock::now();

    Logger::Instance().Info("Routes saved to disk: " + std::to_string(snapshot.size()) + " routes");
}

bool RouteController::AddRoute(const std::string& ip, const std::string& processName) {
    return AddRouteWithMask(ip, 32, processName);
}

bool RouteController::AddRouteWithMask(const std::string& ip, int prefixLength, const std::string& processName) {
    if (!Utils::IsValidIPv4(ip)) return false;

    std::lock_guard<std::mutex> lock(routesMutex);

    std::string routeKey = ip + "/" + std::to_string(prefixLength);

    if (routes.size() >= Constants::MAX_ROUTES) {
        CleanupOldRoutes();
    }

    auto it = routes.find(routeKey);
    if (it != routes.end()) {
        it->second->refCount++;
        Logger::Instance().Info("Route already exists, incrementing ref count: " + routeKey + " (refs: " + std::to_string(it->second->refCount.load()) + ")");
        return true;
    }

    if (AddSystemRouteWithMask(ip, prefixLength)) {
        auto routeInfo = std::make_unique<RouteInfo>(ip, processName);
        routeInfo->prefixLength = prefixLength;
        routes[routeKey] = std::move(routeInfo);
        Logger::Instance().Info("Added new route: " + routeKey + " for process: " + processName);
        routesDirty = true;  // Mark as dirty instead of saving immediately
        return true;
    }

    return false;
}

bool RouteController::RemoveRoute(const std::string& ip) {
    return RemoveRouteWithMask(ip, 32);
}

bool RouteController::RemoveRouteWithMask(const std::string& ip, int prefixLength) {
    std::lock_guard<std::mutex> lock(routesMutex);

    std::string routeKey = ip + "/" + std::to_string(prefixLength);
    auto it = routes.find(routeKey);
    if (it == routes.end()) return false;

    if (--it->second->refCount <= 0) {
        if (RemoveSystemRouteWithMask(ip, prefixLength)) {
            Logger::Instance().Info("Removed route: " + routeKey);
            routes.erase(it);
            routesDirty = true;  // Mark as dirty instead of saving immediately
            return true;
        }
    }

    return true;
}

void RouteController::CleanupAllRoutes() {
    Logger::Instance().Info("CleanupAllRoutes - Starting cleanup of all routes");
    std::lock_guard<std::mutex> lock(routesMutex);

    for (const auto& [routeKey, route] : routes) {
        Logger::Instance().Info("Removing Windows route for: " + routeKey);
        if (!RemoveSystemRouteWithMask(route->ip, route->prefixLength)) {
            Logger::Instance().Error("Failed to remove Windows route for: " + routeKey);
        }
    }

    routes.clear();
    routesDirty = true;  // Mark as dirty

    // Force immediate save for cleanup
    SaveRoutesToDisk();

    Logger::Instance().Info("CleanupAllRoutes - Completed, all routes removed");
}

void RouteController::CleanupOldRoutes() {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(Constants::ROUTE_CLEANUP_HOURS);

    bool anyRemoved = false;
    for (auto it = routes.begin(); it != routes.end();) {
        if (it->second->createdAt < cutoff) {
            RemoveSystemRouteWithMask(it->second->ip, it->second->prefixLength);
            it = routes.erase(it);
            anyRemoved = true;
        }
        else {
            ++it;
        }
    }

    if (anyRemoved) {
        routesDirty = true;
    }
}

size_t RouteController::GetRouteCount() const {
    std::lock_guard<std::mutex> lock(routesMutex);
    return routes.size();
}

std::vector<RouteInfo> RouteController::GetActiveRoutes() const {
    std::lock_guard<std::mutex> lock(routesMutex);
    std::vector<RouteInfo> result;

    for (auto& [routeKey, route] : routes) {
        result.push_back(*route);
    }

    std::sort(result.begin(), result.end(),
        [](const RouteInfo& a, const RouteInfo& b) {
            return a.createdAt > b.createdAt;
        });

    return result;
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
        Logger::Instance().Error("Invalid destination IP: " + ip);
        return false;
    }

    nextHop.si_family = AF_INET;
    if (inet_pton(AF_INET, config.gatewayIp.c_str(), &nextHop.Ipv4.sin_addr) != 1) {
        Logger::Instance().Error("Invalid gateway IP: " + config.gatewayIp);
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
            Logger::Instance().Error("GetBestInterface failed: " + std::to_string(result));
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

    Logger::Instance().Debug("Adding route via CreateIpForwardEntry2: " + ip + "/" + std::to_string(prefixLength) +
        " -> " + config.gatewayIp +
        " (interface: " + std::to_string(bestInterface) + ")");

    DWORD result = CreateIpForwardEntry2(&route);

    if (result == NO_ERROR) {
        Logger::Instance().Info("Successfully added route: " + ip + "/" + std::to_string(prefixLength) + " -> " + config.gatewayIp);
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        Logger::Instance().Debug("Route already exists: " + ip + "/" + std::to_string(prefixLength));
        return true;
    }
    else {
        Logger::Instance().Error("CreateIpForwardEntry2 failed: " + std::to_string(result));

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
        Logger::Instance().Error("Invalid IP address: " + ip);
        return false;
    }

    DWORD mask = prefixLength == 0 ? 0 : (0xFFFFFFFF << (32 - prefixLength));
    route.dwForwardMask = htonl(mask);

    route.dwForwardPolicy = 0;
    route.dwForwardNextHop = inet_addr(config.gatewayIp.c_str());

    if (route.dwForwardNextHop == INADDR_NONE) {
        Logger::Instance().Error("Invalid gateway IP: " + config.gatewayIp);
        return false;
    }

    DWORD bestInterface = 0;
    DWORD result = GetBestInterface(route.dwForwardNextHop, &bestInterface);
    if (result != NO_ERROR) {
        Logger::Instance().Error("GetBestInterface failed: " + std::to_string(result));
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
        Logger::Instance().Info("Interface metric: " + std::to_string(iface.Metric) +
            ", using route metric: " + std::to_string(minMetric));
    }
    else {
        Logger::Instance().Warning("GetIpInterfaceEntry failed: " + std::to_string(result) +
            ", using default metric");
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
        Logger::Instance().Info("Successfully added route via old API: " + ip + "/" + std::to_string(prefixLength));
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        Logger::Instance().Debug("Route already exists: " + ip + "/" + std::to_string(prefixLength));
        return true;
    }
    else {
        Logger::Instance().Error("CreateIpForwardEntry failed: " + std::to_string(result));
        return false;
    }
}

bool RouteController::RemoveSystemRoute(const std::string& ip) {
    return RemoveSystemRouteWithMask(ip, 32);
}

bool RouteController::RemoveSystemRouteWithMask(const std::string& ip, int prefixLength) {
    MIB_IPFORWARDROW route;
    ZeroMemory(&route, sizeof(MIB_IPFORWARDROW));

    route.dwForwardDest = inet_addr(ip.c_str());
    if (route.dwForwardDest == INADDR_NONE) {
        Logger::Instance().Error("Invalid IP address: " + ip);
        return false;
    }

    DWORD mask = prefixLength == 0 ? 0 : (0xFFFFFFFF << (32 - prefixLength));
    route.dwForwardMask = htonl(mask);

    route.dwForwardNextHop = inet_addr(config.gatewayIp.c_str());

    ULONG bestInterface;
    if (GetBestInterface(route.dwForwardNextHop, &bestInterface) == NO_ERROR) {
        route.dwForwardIfIndex = bestInterface;
    }

    DWORD result = DeleteIpForwardEntry(&route);

    if (result == NO_ERROR) {
        Logger::Instance().Debug("Successfully removed route via API: " + ip + "/" + std::to_string(prefixLength));
        return true;
    }
    else if (result == ERROR_NOT_FOUND) {
        Logger::Instance().Debug("Route not found: " + ip + "/" + std::to_string(prefixLength));
        return true;
    }
    else {
        Logger::Instance().Error("Failed to remove route via API: " + ip + "/" + std::to_string(prefixLength) + ", error: " + std::to_string(result));
        return false;
    }
}

void RouteController::VerifyRoutesThreadFunc() {
    Logger::Instance().Info("RouteController verify thread started");

    try {
        while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
            // Interruptible sleep - check shutdown every second
            for (int i = 0; i < Constants::ROUTE_VERIFY_INTERVAL_SEC &&
                running.load() && !ShutdownCoordinator::Instance().isShuttingDown; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            // Exit immediately if shutting down
            if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }

            if (!IsGatewayReachable()) {
                InvalidateInterfaceCache();
                continue;
            }

            // Create a snapshot of routes to verify without holding lock too long
            std::vector<std::pair<std::string, int>> routesToVerify;
            {
                std::lock_guard<std::mutex> lock(routesMutex);
                // Check shutdown again after acquiring lock
                if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                    break;
                }

                for (const auto& [routeKey, route] : routes) {
                    routesToVerify.emplace_back(route->ip, route->prefixLength);
                }
            }

            // Verify routes without holding the lock
            for (const auto& [ip, prefixLength] : routesToVerify) {
                // Check shutdown before each route verification
                if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                    Logger::Instance().Info("Route verification interrupted by shutdown");
                    break;
                }

                AddSystemRouteWithMask(ip, prefixLength);
            }
        }
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("VerifyRoutesThreadFunc exception: " + std::string(e.what()));
    }

    // Log AFTER all work is done, right before thread exits
    Logger::Instance().Info("RouteController verify thread exiting");
}

void RouteController::SaveRoutesToDisk() {
    std::lock_guard<std::mutex> lock(routesMutex);

    std::ofstream file(Constants::STATE_FILE + ".tmp");
    if (!file.is_open()) return;

    file << "version=2\n";

    auto now = std::chrono::system_clock::now();
    auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    file << "timestamp=" << nowSeconds << "\n";

    for (auto& [routeKey, route] : routes) {
        auto createdSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            route->createdAt.time_since_epoch()).count();

        file << "route=" << route->ip << "," << route->processName << ","
            << createdSeconds << "," << route->prefixLength << "\n";
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
    while (std::getline(file, line)) {
        if (line.substr(0, 6) == "route=") {
            std::string routeData = line.substr(6);
            auto parts = Utils::SplitString(routeData, ',');
            if (parts.size() >= 2) {
                std::string ip = parts[0];
                std::string process = parts[1];
                int prefixLength = 32;

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
                        Logger::Instance().Warning("Failed to parse timestamp for route: " + ip);
                    }
                }

                if (parts.size() >= 4) {
                    try {
                        prefixLength = std::stoi(parts[3]);
                    }
                    catch (...) {
                        Logger::Instance().Warning("Failed to parse prefix length for route: " + ip);
                    }
                }

                if (AddSystemRouteWithMask(ip, prefixLength)) {
                    std::string routeKey = ip + "/" + std::to_string(prefixLength);
                    auto routeInfo = std::make_unique<RouteInfo>(ip, process);
                    routeInfo->prefixLength = prefixLength;
                    routeInfo->createdAt = createdAt;
                    routes[routeKey] = std::move(routeInfo);
                }
            }
        }
    }

    // Reset dirty flag after loading
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
            Logger::Instance().Info("Skipping disabled service: " + service.name);
            continue;
        }

        Logger::Instance().Info("Processing " + service.name + " ranges");
        for (const auto& range : service.ranges) {
            if (range.find('/') != std::string::npos) {
                if (AddCIDRRoute(range, service.name)) {
                    totalRoutes++;
                }
            }
            else {
                if (AddRoute(range, "Preload-" + service.name)) {
                    totalRoutes++;
                }
            }
        }
    }
    Logger::Instance().Info("PreloadRoutes - Completed, added " + std::to_string(totalRoutes) + " routes");
}

std::vector<RouteController::PreloadService> RouteController::LoadPreloadConfig() {
    std::vector<PreloadService> services;

    std::string configPath = Utils::GetCurrentDirectory() + "\\preload_ips.json";

    if (!Utils::FileExists(configPath)) {
        CreateDefaultPreloadConfig(configPath);
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        Logger::Instance().Error("Failed to open preload config: " + configPath);
        return GetDefaultPreloadServices();
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;

    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        Logger::Instance().Error("Failed to parse preload config: " + errors);
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

    Logger::Instance().Info("Loaded " + std::to_string(services.size()) + " services from preload config");
    return services;
}

void RouteController::CreateDefaultPreloadConfig(const std::string& path) {
    std::string sourceFile = Utils::GetCurrentDirectory() + "\\config\\preload_ips.json";

    if (Utils::FileExists(sourceFile)) {
        std::ifstream src(sourceFile, std::ios::binary);
        if (src.is_open()) {
            std::ofstream dst(path, std::ios::binary);
            if (dst.is_open()) {
                dst << src.rdbuf();
                Logger::Instance().Info("Copied default preload config from: " + sourceFile);
                return;
            }
        }
    }

    Logger::Instance().Warning("Could not copy default config from " + sourceFile + ", using fallback");

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
        Logger::Instance().Info("Created minimal fallback preload config: " + path);
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

    Logger::Instance().Info("Adding CIDR route: " + cidr + " for " + service);

    return AddRouteWithMask(baseIp, prefixLen, "Preload-" + service);
}