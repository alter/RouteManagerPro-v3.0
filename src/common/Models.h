// src/common/Models.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>

struct ProcessInfo {
    std::wstring name;
    std::wstring executablePath;
    DWORD pid;
    bool isSelected;
    bool isGame;
    bool isDiscord;
};

struct RouteInfo {
    std::string ip;
    std::string processName;
    std::atomic<int> refCount;
    std::chrono::system_clock::time_point createdAt;

    RouteInfo() : refCount(1), createdAt(std::chrono::system_clock::now()) {}
    RouteInfo(const std::string& ip, const std::string& process)
        : ip(ip), processName(process), refCount(1), createdAt(std::chrono::system_clock::now()) {
    }

    RouteInfo(const RouteInfo& other)
        : ip(other.ip), processName(other.processName),
        refCount(other.refCount.load()), createdAt(other.createdAt) {
    }

    RouteInfo& operator=(const RouteInfo& other) {
        if (this != &other) {
            ip = other.ip;
            processName = other.processName;
            refCount = other.refCount.load();
            createdAt = other.createdAt;
        }
        return *this;
    }
};

struct NetworkEvent {
    std::string processName;
    std::string remoteIp;
    UINT16 remotePort;
    std::string protocol;
    std::chrono::system_clock::time_point timestamp;

    NetworkEvent() : remotePort(0), timestamp(std::chrono::system_clock::now()) {}
};

struct ServiceConfig {
    std::string gatewayIp = "10.200.210.1";
    int metric = 1;
    std::vector<std::string> selectedProcesses;
    bool startMinimized = true;
    bool startWithWindows = true;
    bool aiPreloadEnabled = false;
};

struct ServiceStatus {
    bool isRunning;
    bool monitorActive;
    size_t activeRoutes;
    size_t memoryUsageMB;
    std::chrono::seconds uptime;
};

enum class PacketPriority {
    Discord = 0,
    Gaming = 1,
    Development = 2,
    Normal = 3
};

struct PacketInfo {
    UINT64 flowId;
    std::string srcIp;
    std::string dstIp;
    UINT16 srcPort;
    UINT16 dstPort;
    UINT8 protocol;
    std::string processPath;
    PacketPriority priority;
};