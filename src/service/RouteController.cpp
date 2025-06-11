// src/service/RouteController.cpp
#include "RouteController.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <chrono>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

RouteController::RouteController(const ServiceConfig& cfg) : config(cfg), running(true),
lastSaveTime(std::chrono::steady_clock::now()) {
    LoadRoutesFromDisk();
    verifyThread = std::thread(&RouteController::VerifyRoutesThreadFunc, this);
    if (config.aiPreloadEnabled) {
        PreloadAIRoutes();
    }
}

RouteController::~RouteController() {
    running = false;
    if (verifyThread.joinable()) {
        verifyThread.join();
    }
    SaveRoutesToDisk();
}

bool RouteController::AddRoute(const std::string& ip, const std::string& processName) {
    if (!Utils::IsValidIPv4(ip)) return false;

    std::lock_guard<std::mutex> lock(routesMutex);

    if (routes.size() >= Constants::MAX_ROUTES) {
        CleanupOldRoutes();
    }

    auto it = routes.find(ip);
    if (it != routes.end()) {
        it->second->refCount++;
        return true;
    }

    if (AddSystemRoute(ip)) {
        routes[ip] = std::make_unique<RouteInfo>(ip, processName);
        Logger::Instance().Info("Added new route: " + ip + " for process: " + processName);
        return true;
    }

    return false;
}

bool RouteController::RemoveRoute(const std::string& ip) {
    std::lock_guard<std::mutex> lock(routesMutex);

    auto it = routes.find(ip);
    if (it == routes.end()) return false;

    if (--it->second->refCount <= 0) {
        if (RemoveSystemRoute(ip)) {
            Logger::Instance().Info("Removed route: " + ip);
            routes.erase(it);
            return true;
        }
    }

    return true;
}

void RouteController::CleanupAllRoutes() {
    Logger::Instance().Info("CleanupAllRoutes - Starting cleanup of all routes");
    std::lock_guard<std::mutex> lock(routesMutex);

    for (const auto& [ip, route] : routes) {
        Logger::Instance().Debug("Removing Windows route for: " + ip);
        if (!RemoveSystemRoute(ip)) {
            Logger::Instance().Error("Failed to remove Windows route for: " + ip);
        }
    }

    routes.clear();
    SaveRoutesToDisk();

    Logger::Instance().Info("CleanupAllRoutes - Completed, all routes removed");
}

void RouteController::CleanupOldRoutes() {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(Constants::ROUTE_CLEANUP_HOURS);

    for (auto it = routes.begin(); it != routes.end();) {
        if (it->second->createdAt < cutoff) {
            RemoveSystemRoute(it->first);
            it = routes.erase(it);
        }
        else {
            ++it;
        }
    }
}

size_t RouteController::GetRouteCount() const {
    std::lock_guard<std::mutex> lock(routesMutex);
    return routes.size();
}

std::vector<RouteInfo> RouteController::GetActiveRoutes() const {
    std::lock_guard<std::mutex> lock(routesMutex);
    std::vector<RouteInfo> result;
    result.reserve(routes.size());

    for (const auto& [ip, route] : routes) {
        result.push_back(*route);
    }

    return result;
}

bool RouteController::RouteExists(const std::string& ip) const {
    std::lock_guard<std::mutex> lock(routesMutex);
    return routes.find(ip) != routes.end();
}

bool RouteController::AddSystemRoute(const std::string& ip) {
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
    DWORD result = GetBestInterface(nextHop.Ipv4.sin_addr.s_addr, &bestInterface);
    if (result != NO_ERROR) {
        Logger::Instance().Error("GetBestInterface failed: " + std::to_string(result));
        return false;
    }

    route.InterfaceIndex = bestInterface;
    route.DestinationPrefix.Prefix = destAddr;
    route.DestinationPrefix.PrefixLength = 32;
    route.NextHop = nextHop;
    route.Protocol = MIB_IPPROTO_NETMGMT;
    route.Metric = config.metric;

    result = CreateIpForwardEntry2(&route);

    if (result == NO_ERROR) {
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        return true;
    }
    else {
        Logger::Instance().Error("CreateIpForwardEntry2 failed: " + std::to_string(result));

        if (result == ERROR_NOT_FOUND || result == ERROR_INVALID_FUNCTION) {
            return AddSystemRouteOldAPI(ip);
        }

        return false;
    }
}

bool RouteController::AddSystemRouteOldAPI(const std::string& ip) {
    MIB_IPFORWARDROW route = { 0 };

    route.dwForwardDest = inet_addr(ip.c_str());
    if (route.dwForwardDest == INADDR_NONE) {
        Logger::Instance().Error("Invalid IP address: " + ip);
        return false;
    }

    route.dwForwardMask = 0xFFFFFFFF;
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
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        return true;
    }
    else {
        Logger::Instance().Error("CreateIpForwardEntry failed: " + std::to_string(result));
        return false;
    }
}

bool RouteController::RemoveSystemRoute(const std::string& ip) {
    MIB_IPFORWARDROW route;
    ZeroMemory(&route, sizeof(MIB_IPFORWARDROW));

    route.dwForwardDest = inet_addr(ip.c_str());
    if (route.dwForwardDest == INADDR_NONE) {
        Logger::Instance().Error("Invalid IP address: " + ip);
        return false;
    }

    route.dwForwardMask = inet_addr("255.255.255.255");
    route.dwForwardNextHop = inet_addr(config.gatewayIp.c_str());

    ULONG bestInterface;
    if (GetBestInterface(route.dwForwardNextHop, &bestInterface) == NO_ERROR) {
        route.dwForwardIfIndex = bestInterface;
    }

    DWORD result = DeleteIpForwardEntry(&route);

    if (result == NO_ERROR) {
        return true;
    }
    else if (result == ERROR_NOT_FOUND) {
        return true;
    }
    else {
        Logger::Instance().Error("Failed to remove route via API: " + ip + ", error: " + std::to_string(result));
        return false;
    }
}

void RouteController::VerifyRoutesThreadFunc() {
    while (running.load()) {
        for (int i = 0; i < Constants::ROUTE_VERIFY_INTERVAL_SEC; i++) {
            if (!running.load()) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!running.load()) break;

        if (!IsGatewayReachable()) {
            continue;
        }

        std::lock_guard<std::mutex> lock(routesMutex);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - lastSaveTime).count();

        if (elapsed >= 10) {
            SaveRoutesToDisk();
            lastSaveTime = now;
        }

        for (const auto& [ip, route] : routes) {
            AddSystemRoute(ip);
        }
    }
}

void RouteController::SaveRoutesToDisk() {
    if (routes.empty()) return;

    Logger::Instance().Debug("Saving " + std::to_string(routes.size()) + " routes to disk");

    std::ofstream file(Constants::STATE_FILE + ".tmp");
    if (!file.is_open()) return;

    file << "version=1\n";
    file << "timestamp=" << std::chrono::system_clock::now().time_since_epoch().count() << "\n";

    for (const auto& [ip, route] : routes) {
        file << "route=" << route->ip << "," << route->processName << ","
            << route->createdAt.time_since_epoch().count() << "\n";
    }

    file.close();

    if (std::rename((Constants::STATE_FILE + ".tmp").c_str(), Constants::STATE_FILE.c_str()) != 0) {
        MoveFileExA((Constants::STATE_FILE + ".tmp").c_str(), Constants::STATE_FILE.c_str(),
            MOVEFILE_REPLACE_EXISTING);
    }
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

                if (AddSystemRoute(ip)) {
                    routes[ip] = std::make_unique<RouteInfo>(ip, process);
                }
            }
        }
    }

    Logger::Instance().Info("Loaded " + std::to_string(routes.size()) + " routes from disk");
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
    Logger::Instance().Info("PreloadAIRoutes - Starting AI routes preloading");
    auto aiRanges = GetAIServiceRanges();
    int addedCount = 0;
    int skippedCount = 0;

    for (const auto& service : aiRanges) {
        for (const auto& range : service.ranges) {
            if (range.find('/') != std::string::npos) {
                auto result = AddCIDRRoutes(range, service.service);
                addedCount += result.first;
                skippedCount += result.second;
            }
            else {
                if (!RouteExists(range)) {
                    if (AddRoute(range, "AI-" + service.service)) {
                        addedCount++;
                    }
                }
                else {
                    skippedCount++;
                }
            }
        }
    }

    Logger::Instance().Info("PreloadAIRoutes - Completed: " + std::to_string(addedCount) +
        " added, " + std::to_string(skippedCount) + " already existed");
}

std::vector<RouteController::AIServiceRange> RouteController::GetAIServiceRanges() {
    return {
        {"Claude (Anthropic)", {
            "160.79.104.0/23",
            "160.79.104.10"
        }},
        {"ChatGPT (OpenAI)", {
            "23.102.140.112/28",
            "13.66.11.96/28",
            "104.210.133.240/28",
            "23.98.142.176/28",
            "40.84.180.224/28",
            "52.230.152.0/24",
            "52.233.106.0/24"
        }},
        {"Cloudflare CDN", {
            "104.16.0.0/12",
            "162.158.0.0/15",
            "172.64.0.0/13",
            "173.245.48.0/20"
        }}
    };
}

std::pair<int, int> RouteController::AddCIDRRoutes(const std::string& cidr, const std::string& service) {
    int added = 0;
    int skipped = 0;

    size_t slashPos = cidr.find('/');
    if (slashPos == std::string::npos) return { 0, 0 };

    std::string baseIp = cidr.substr(0, slashPos);
    int prefixLen = std::stoi(cidr.substr(slashPos + 1));

    if (prefixLen >= 24) {
        if (!RouteExists(baseIp)) {
            if (AddRoute(baseIp, "AI-" + service)) {
                added++;
            }
        }
        else {
            skipped++;
        }
        return { added, skipped };
    }

    ULONG addr = inet_addr(baseIp.c_str());
    addr = ntohl(addr);

    ULONG firstAddr = addr & (0xFFFFFFFF << (32 - prefixLen));
    ULONG lastAddr = firstAddr | (0xFFFFFFFF >> prefixLen);

    in_addr inAddr;

    inAddr.s_addr = htonl(firstAddr);
    std::string firstIp = inet_ntoa(inAddr);
    if (!RouteExists(firstIp)) {
        if (AddRoute(firstIp, "AI-" + service)) {
            added++;
        }
    }
    else {
        skipped++;
    }

    inAddr.s_addr = htonl(firstAddr + 1);
    std::string secondIp = inet_ntoa(inAddr);
    if (!RouteExists(secondIp)) {
        if (AddRoute(secondIp, "AI-" + service)) {
            added++;
        }
    }
    else {
        skipped++;
    }

    inAddr.s_addr = htonl(lastAddr - 1);
    std::string lastIp = inet_ntoa(inAddr);
    if (!RouteExists(lastIp)) {
        if (AddRoute(lastIp, "AI-" + service)) {
            added++;
        }
    }
    else {
        skipped++;
    }

    return { added, skipped };
}