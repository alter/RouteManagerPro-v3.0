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
#include <chrono>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

RouteController::RouteController(const ServiceConfig& cfg) : config(cfg), running(true) {
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
        Logger::Instance().Info("Route already exists, incrementing ref count: " + ip + " (refs: " + std::to_string(it->second->refCount.load()) + ")");
        return true;
    }

    if (AddSystemRoute(ip)) {
        routes[ip] = std::make_unique<RouteInfo>(ip, processName);
        Logger::Instance().Info("Added new route: " + ip + " for process: " + processName);
        SaveRoutesToDisk();
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
            SaveRoutesToDisk();
            return true;
        }
    }

    return true;
}

void RouteController::CleanupAllRoutes() {
    Logger::Instance().Info("CleanupAllRoutes - Starting cleanup of all routes");
    std::lock_guard<std::mutex> lock(routesMutex);

    for (auto it = routes.begin(); it != routes.end(); ++it) {
        std::string ip = it->first;
        Logger::Instance().Info("Removing Windows route for: " + ip);
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

    for (auto it = routes.begin(); it != routes.end(); ++it) {
        result.push_back(*(it->second));
    }

    return result;
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

    Logger::Instance().Debug("Adding route via CreateIpForwardEntry2: " + ip +
        " -> " + config.gatewayIp +
        " (interface: " + std::to_string(bestInterface) + ")");

    result = CreateIpForwardEntry2(&route);

    if (result == NO_ERROR) {
        Logger::Instance().Info("Successfully added route: " + ip + " -> " + config.gatewayIp);
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        Logger::Instance().Debug("Route already exists: " + ip);
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
    Logger::Instance().Info("Falling back to old API for compatibility");

    MIB_IPFORWARDROW route = { 0 };

    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        Logger::Instance().Error("Invalid IP address: " + ip);
        return false;
    }
    route.dwForwardDest = addr.s_addr;

    route.dwForwardMask = 0xFFFFFFFF;
    route.dwForwardPolicy = 0;

    struct in_addr gwAddr;
    if (inet_pton(AF_INET, config.gatewayIp.c_str(), &gwAddr) != 1) {
        Logger::Instance().Error("Invalid gateway IP: " + config.gatewayIp);
        return false;
    }
    route.dwForwardNextHop = gwAddr.s_addr;

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
        Logger::Instance().Info("Successfully added route via old API: " + ip);
        return true;
    }
    else if (result == ERROR_OBJECT_ALREADY_EXISTS) {
        Logger::Instance().Debug("Route already exists: " + ip);
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

    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        Logger::Instance().Error("Invalid IP address: " + ip);
        return false;
    }
    route.dwForwardDest = addr.s_addr;

    struct in_addr maskAddr;
    inet_pton(AF_INET, "255.255.255.255", &maskAddr);
    route.dwForwardMask = maskAddr.s_addr;

    struct in_addr gwAddr;
    inet_pton(AF_INET, config.gatewayIp.c_str(), &gwAddr);
    route.dwForwardNextHop = gwAddr.s_addr;

    ULONG bestInterface;
    if (GetBestInterface(route.dwForwardNextHop, &bestInterface) == NO_ERROR) {
        route.dwForwardIfIndex = bestInterface;
    }

    DWORD result = DeleteIpForwardEntry(&route);

    if (result == NO_ERROR) {
        Logger::Instance().Debug("Successfully removed route via API: " + ip);
        return true;
    }
    else if (result == ERROR_NOT_FOUND) {
        Logger::Instance().Debug("Route not found: " + ip);
        return true;
    }
    else {
        Logger::Instance().Error("Failed to remove route via API: " + ip + ", error: " + std::to_string(result));
        return false;
    }
}

void RouteController::VerifyRoutesThreadFunc() {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(Constants::ROUTE_VERIFY_INTERVAL_SEC));

        if (!IsGatewayReachable()) {
            continue;
        }

        std::lock_guard<std::mutex> lock(routesMutex);
        for (auto it = routes.begin(); it != routes.end(); ++it) {
            AddSystemRoute(it->first);
        }
    }
}

void RouteController::SaveRoutesToDisk() {
    std::ofstream file(Constants::STATE_FILE + ".tmp");
    if (!file.is_open()) return;

    file << "version=1\n";
    file << "timestamp=" << std::chrono::system_clock::now().time_since_epoch().count() << "\n";

    for (auto it = routes.begin(); it != routes.end(); ++it) {
        RouteInfo* route = it->second.get();
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
}

bool RouteController::IsGatewayReachable() {
    struct in_addr addr;
    if (inet_pton(AF_INET, config.gatewayIp.c_str(), &addr) != 1) {
        return false;
    }

    ULONG destAddr = addr.s_addr;
    ULONG srcAddr = INADDR_ANY;
    ULONG bestIfIndex;

    if (GetBestInterface(destAddr, &bestIfIndex) != NO_ERROR) {
        return false;
    }

    return true;
}

void RouteController::PreloadAIRoutes() {
    auto aiRanges = GetAIServiceRanges();

    for (const auto& service : aiRanges) {
        for (const auto& range : service.ranges) {
            if (range.find('/') != std::string::npos) {
                AddCIDRRoutes(range, service.service);
            }
            else {
                AddRoute(range, "AI-" + service.service);
            }
        }
    }
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

void RouteController::AddCIDRRoutes(const std::string& cidr, const std::string& service) {
    size_t slashPos = cidr.find('/');
    if (slashPos == std::string::npos) return;

    std::string baseIp = cidr.substr(0, slashPos);
    int prefixLen = std::stoi(cidr.substr(slashPos + 1));

    if (prefixLen >= 24) {
        AddRoute(baseIp, "AI-" + service);
        return;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, baseIp.c_str(), &addr) != 1) {
        return;
    }

    ULONG ipAddr = ntohl(addr.s_addr);

    ULONG firstAddr = ipAddr & (0xFFFFFFFF << (32 - prefixLen));
    ULONG lastAddr = firstAddr | (0xFFFFFFFF >> prefixLen);

    in_addr inAddr;
    char ipStr[INET_ADDRSTRLEN];

    inAddr.s_addr = htonl(firstAddr);
    inet_ntop(AF_INET, &inAddr, ipStr, INET_ADDRSTRLEN);
    AddRoute(ipStr, "AI-" + service);

    inAddr.s_addr = htonl(firstAddr + 1);
    inet_ntop(AF_INET, &inAddr, ipStr, INET_ADDRSTRLEN);
    AddRoute(ipStr, "AI-" + service);

    inAddr.s_addr = htonl(lastAddr - 1);
    inet_ntop(AF_INET, &inAddr, ipStr, INET_ADDRSTRLEN);
    AddRoute(ipStr, "AI-" + service);
}