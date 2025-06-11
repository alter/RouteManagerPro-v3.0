// src/common/IPCSerializer.cpp
#include "IPCProtocol.h"
#include <cstring>

std::vector<uint8_t> IPCSerializer::SerializeServiceStatus(const ServiceStatus& status) {
    std::vector<uint8_t> data;
    data.resize(sizeof(bool) * 2 + sizeof(size_t) * 2 + sizeof(int64_t));

    size_t offset = 0;
    memcpy(data.data() + offset, &status.isRunning, sizeof(bool));
    offset += sizeof(bool);

    memcpy(data.data() + offset, &status.monitorActive, sizeof(bool));
    offset += sizeof(bool);

    memcpy(data.data() + offset, &status.activeRoutes, sizeof(size_t));
    offset += sizeof(size_t);

    memcpy(data.data() + offset, &status.memoryUsageMB, sizeof(size_t));
    offset += sizeof(size_t);

    int64_t uptimeCount = status.uptime.count();
    memcpy(data.data() + offset, &uptimeCount, sizeof(int64_t));

    return data;
}

ServiceStatus IPCSerializer::DeserializeServiceStatus(const std::vector<uint8_t>& data) {
    ServiceStatus status;
    if (data.size() < sizeof(bool) * 2 + sizeof(size_t) * 2 + sizeof(int64_t)) {
        return status;
    }

    size_t offset = 0;
    memcpy(&status.isRunning, data.data() + offset, sizeof(bool));
    offset += sizeof(bool);

    memcpy(&status.monitorActive, data.data() + offset, sizeof(bool));
    offset += sizeof(bool);

    memcpy(&status.activeRoutes, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    memcpy(&status.memoryUsageMB, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    int64_t uptimeCount;
    memcpy(&uptimeCount, data.data() + offset, sizeof(int64_t));
    status.uptime = std::chrono::seconds(uptimeCount);

    return status;
}

std::vector<uint8_t> IPCSerializer::SerializeServiceConfig(const ServiceConfig& config) {
    std::vector<uint8_t> data;

    size_t gatewaySize = config.gatewayIp.size();
    size_t processCount = config.selectedProcesses.size();
    size_t totalSize = sizeof(size_t) + gatewaySize + sizeof(int) + sizeof(bool) * 3 +
        sizeof(size_t) + processCount * sizeof(size_t);

    for (const auto& process : config.selectedProcesses) {
        totalSize += process.size();
    }

    data.resize(totalSize);
    size_t offset = 0;

    memcpy(data.data() + offset, &gatewaySize, sizeof(size_t));
    offset += sizeof(size_t);

    memcpy(data.data() + offset, config.gatewayIp.c_str(), gatewaySize);
    offset += gatewaySize;

    memcpy(data.data() + offset, &config.metric, sizeof(int));
    offset += sizeof(int);

    memcpy(data.data() + offset, &config.startMinimized, sizeof(bool));
    offset += sizeof(bool);

    memcpy(data.data() + offset, &config.startWithWindows, sizeof(bool));
    offset += sizeof(bool);

    memcpy(data.data() + offset, &config.aiPreloadEnabled, sizeof(bool));
    offset += sizeof(bool);

    memcpy(data.data() + offset, &processCount, sizeof(size_t));
    offset += sizeof(size_t);

    for (const auto& process : config.selectedProcesses) {
        size_t len = process.size();
        memcpy(data.data() + offset, &len, sizeof(size_t));
        offset += sizeof(size_t);

        memcpy(data.data() + offset, process.c_str(), len);
        offset += len;
    }

    return data;
}

ServiceConfig IPCSerializer::DeserializeServiceConfig(const std::vector<uint8_t>& data) {
    ServiceConfig config;
    if (data.size() < sizeof(size_t)) return config;

    size_t offset = 0;
    size_t gatewaySize;
    memcpy(&gatewaySize, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    if (offset + gatewaySize > data.size()) return config;
    config.gatewayIp.assign(reinterpret_cast<const char*>(data.data() + offset), gatewaySize);
    offset += gatewaySize;

    if (offset + sizeof(int) + sizeof(bool) * 3 + sizeof(size_t) > data.size()) return config;

    memcpy(&config.metric, data.data() + offset, sizeof(int));
    offset += sizeof(int);

    memcpy(&config.startMinimized, data.data() + offset, sizeof(bool));
    offset += sizeof(bool);

    memcpy(&config.startWithWindows, data.data() + offset, sizeof(bool));
    offset += sizeof(bool);

    memcpy(&config.aiPreloadEnabled, data.data() + offset, sizeof(bool));
    offset += sizeof(bool);

    size_t processCount;
    memcpy(&processCount, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    for (size_t i = 0; i < processCount; i++) {
        if (offset + sizeof(size_t) > data.size()) break;

        size_t len;
        memcpy(&len, data.data() + offset, sizeof(size_t));
        offset += sizeof(size_t);

        if (offset + len > data.size()) break;

        std::string process(reinterpret_cast<const char*>(data.data() + offset), len);
        config.selectedProcesses.push_back(process);
        offset += len;
    }

    return config;
}

std::vector<uint8_t> IPCSerializer::SerializeProcessList(const std::vector<ProcessInfo>& processes) {
    std::vector<uint8_t> data;

    size_t totalSize = sizeof(size_t);
    for (const auto& process : processes) {
        totalSize += sizeof(size_t) * 2 + process.name.size() * sizeof(wchar_t) +
            process.executablePath.size() * sizeof(wchar_t) +
            sizeof(DWORD) + sizeof(bool) * 3;
    }

    data.resize(totalSize);
    size_t offset = 0;

    size_t count = processes.size();
    memcpy(data.data() + offset, &count, sizeof(size_t));
    offset += sizeof(size_t);

    for (const auto& process : processes) {
        size_t nameLen = process.name.size();
        memcpy(data.data() + offset, &nameLen, sizeof(size_t));
        offset += sizeof(size_t);

        memcpy(data.data() + offset, process.name.c_str(), nameLen * sizeof(wchar_t));
        offset += nameLen * sizeof(wchar_t);

        size_t pathLen = process.executablePath.size();
        memcpy(data.data() + offset, &pathLen, sizeof(size_t));
        offset += sizeof(size_t);

        memcpy(data.data() + offset, process.executablePath.c_str(), pathLen * sizeof(wchar_t));
        offset += pathLen * sizeof(wchar_t);

        memcpy(data.data() + offset, &process.pid, sizeof(DWORD));
        offset += sizeof(DWORD);

        memcpy(data.data() + offset, &process.isSelected, sizeof(bool));
        offset += sizeof(bool);

        memcpy(data.data() + offset, &process.isGame, sizeof(bool));
        offset += sizeof(bool);

        memcpy(data.data() + offset, &process.isDiscord, sizeof(bool));
        offset += sizeof(bool);
    }

    return data;
}

std::vector<ProcessInfo> IPCSerializer::DeserializeProcessList(const std::vector<uint8_t>& data) {
    std::vector<ProcessInfo> processes;
    if (data.size() < sizeof(size_t)) return processes;

    size_t offset = 0;
    size_t count;
    memcpy(&count, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    for (size_t i = 0; i < count; i++) {
        ProcessInfo process;

        if (offset + sizeof(size_t) > data.size()) break;

        size_t nameLen;
        memcpy(&nameLen, data.data() + offset, sizeof(size_t));
        offset += sizeof(size_t);

        if (offset + nameLen * sizeof(wchar_t) > data.size()) break;

        process.name.assign(reinterpret_cast<const wchar_t*>(data.data() + offset), nameLen);
        offset += nameLen * sizeof(wchar_t);

        if (offset + sizeof(size_t) > data.size()) break;

        size_t pathLen;
        memcpy(&pathLen, data.data() + offset, sizeof(size_t));
        offset += sizeof(size_t);

        if (offset + pathLen * sizeof(wchar_t) > data.size()) break;

        process.executablePath.assign(reinterpret_cast<const wchar_t*>(data.data() + offset), pathLen);
        offset += pathLen * sizeof(wchar_t);

        if (offset + sizeof(DWORD) + sizeof(bool) * 3 > data.size()) break;

        memcpy(&process.pid, data.data() + offset, sizeof(DWORD));
        offset += sizeof(DWORD);

        memcpy(&process.isSelected, data.data() + offset, sizeof(bool));
        offset += sizeof(bool);

        memcpy(&process.isGame, data.data() + offset, sizeof(bool));
        offset += sizeof(bool);

        memcpy(&process.isDiscord, data.data() + offset, sizeof(bool));
        offset += sizeof(bool);

        processes.push_back(process);
    }

    return processes;
}

std::vector<uint8_t> IPCSerializer::SerializeRouteList(const std::vector<RouteInfo>& routes) {
    std::vector<uint8_t> data;

    size_t totalSize = sizeof(size_t);
    for (const auto& route : routes) {
        totalSize += sizeof(size_t) * 2 + route.ip.size() + route.processName.size() +
            sizeof(int) + sizeof(int64_t);
    }

    data.resize(totalSize);
    size_t offset = 0;

    size_t count = routes.size();
    memcpy(data.data() + offset, &count, sizeof(size_t));
    offset += sizeof(size_t);

    for (const auto& route : routes) {
        size_t ipLen = route.ip.size();
        memcpy(data.data() + offset, &ipLen, sizeof(size_t));
        offset += sizeof(size_t);

        memcpy(data.data() + offset, route.ip.c_str(), ipLen);
        offset += ipLen;

        size_t processLen = route.processName.size();
        memcpy(data.data() + offset, &processLen, sizeof(size_t));
        offset += sizeof(size_t);

        memcpy(data.data() + offset, route.processName.c_str(), processLen);
        offset += processLen;

        int refCount = route.refCount.load();
        memcpy(data.data() + offset, &refCount, sizeof(int));
        offset += sizeof(int);

        int64_t createdAt = std::chrono::duration_cast<std::chrono::seconds>(
            route.createdAt.time_since_epoch()).count();
        memcpy(data.data() + offset, &createdAt, sizeof(int64_t));
        offset += sizeof(int64_t);
    }

    return data;
}

std::vector<RouteInfo> IPCSerializer::DeserializeRouteList(const std::vector<uint8_t>& data) {
    std::vector<RouteInfo> routes;
    if (data.size() < sizeof(size_t)) return routes;

    size_t offset = 0;
    size_t count;
    memcpy(&count, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    for (size_t i = 0; i < count; i++) {
        if (offset + sizeof(size_t) > data.size()) break;

        size_t ipLen;
        memcpy(&ipLen, data.data() + offset, sizeof(size_t));
        offset += sizeof(size_t);

        if (offset + ipLen > data.size()) break;

        std::string ip(reinterpret_cast<const char*>(data.data() + offset), ipLen);
        offset += ipLen;

        if (offset + sizeof(size_t) > data.size()) break;

        size_t processLen;
        memcpy(&processLen, data.data() + offset, sizeof(size_t));
        offset += sizeof(size_t);

        if (offset + processLen > data.size()) break;

        std::string processName(reinterpret_cast<const char*>(data.data() + offset), processLen);
        offset += processLen;

        if (offset + sizeof(int) + sizeof(int64_t) > data.size()) break;

        RouteInfo route(ip, processName);

        int refCount;
        memcpy(&refCount, data.data() + offset, sizeof(int));
        route.refCount = refCount;
        offset += sizeof(int);

        int64_t createdAt;
        memcpy(&createdAt, data.data() + offset, sizeof(int64_t));
        route.createdAt = std::chrono::system_clock::time_point(std::chrono::seconds(createdAt));
        offset += sizeof(int64_t);

        routes.push_back(route);
    }

    return routes;
}

std::vector<uint8_t> IPCSerializer::SerializeStringList(const std::vector<std::string>& strings) {
    std::vector<uint8_t> data;

    size_t totalSize = sizeof(size_t);
    for (const auto& str : strings) {
        totalSize += sizeof(size_t) + str.size();
    }

    data.resize(totalSize);
    size_t offset = 0;

    size_t count = strings.size();
    memcpy(data.data() + offset, &count, sizeof(size_t));
    offset += sizeof(size_t);

    for (const auto& str : strings) {
        size_t len = str.size();
        memcpy(data.data() + offset, &len, sizeof(size_t));
        offset += sizeof(size_t);

        memcpy(data.data() + offset, str.c_str(), len);
        offset += len;
    }

    return data;
}

std::vector<std::string> IPCSerializer::DeserializeStringList(const std::vector<uint8_t>& data) {
    std::vector<std::string> strings;
    if (data.size() < sizeof(size_t)) return strings;

    size_t offset = 0;
    size_t count;
    memcpy(&count, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);

    for (size_t i = 0; i < count; i++) {
        if (offset + sizeof(size_t) > data.size()) break;

        size_t len;
        memcpy(&len, data.data() + offset, sizeof(size_t));
        offset += sizeof(size_t);

        if (offset + len > data.size()) break;

        strings.emplace_back(reinterpret_cast<const char*>(data.data() + offset), len);
        offset += len;
    }

    return strings;
}