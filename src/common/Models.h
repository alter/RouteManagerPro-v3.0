// src/common/Models.h
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <unordered_map>

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
    int prefixLength;

    RouteInfo() : refCount(1), createdAt(std::chrono::system_clock::now()), prefixLength(32) {}
    RouteInfo(const std::string& ip, const std::string& process)
        : ip(ip), processName(process), refCount(1), createdAt(std::chrono::system_clock::now()), prefixLength(32) {
    }

    RouteInfo(const RouteInfo& other)
        : ip(other.ip), processName(other.processName),
        refCount(other.refCount.load()), createdAt(other.createdAt), prefixLength(other.prefixLength) {
    }

    RouteInfo& operator=(const RouteInfo& other) {
        if (this != &other) {
            ip = other.ip;
            processName = other.processName;
            refCount = other.refCount.load();
            createdAt = other.createdAt;
            prefixLength = other.prefixLength;
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

struct OptimizerSettings {
    int minHostsToAggregate = 2;
    std::unordered_map<int, float> wasteThresholds = {
        {30, 0.75f}, {29, 0.80f}, {28, 0.85f},
        {27, 0.90f}, {26, 0.90f}, {25, 0.92f}, {24, 0.95f}
    };
};

struct ServiceConfig {
    std::string gatewayIp = "10.200.210.1";
    int metric = 1;
    std::vector<std::string> selectedProcesses;
    bool startMinimized = true;
    bool startWithWindows = true;
    bool aiPreloadEnabled = false;
    OptimizerSettings optimizerSettings;
};

struct ServiceStatus {
    bool isRunning;
    bool monitorActive;
    size_t activeRoutes;
    size_t memoryUsageMB;
    std::chrono::seconds uptime;
};