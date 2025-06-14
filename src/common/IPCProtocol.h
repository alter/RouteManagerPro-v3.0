// src/common/IPCProtocol.h
#pragma once
#include <string>
#include <vector>
#include "Models.h"

enum class IPCMessageType {
    GetStatus = 1,
    GetConfig = 2,
    SetConfig = 3,
    GetProcesses = 4,
    SetSelectedProcesses = 5,
    GetRoutes = 6,
    AddRoute = 7,
    RemoveRoute = 8,
    ClearRoutes = 9,
    OptimizeRoutes = 10,
    SetAIPreload = 12
};

struct IPCMessage {
    IPCMessageType type;
    std::vector<uint8_t> data;
};

struct IPCResponse {
    bool success;
    std::vector<uint8_t> data;
    std::string error;
};

class IPCSerializer {
public:
    static std::vector<uint8_t> SerializeServiceStatus(const ServiceStatus& status);
    static ServiceStatus DeserializeServiceStatus(const std::vector<uint8_t>& data);

    static std::vector<uint8_t> SerializeServiceConfig(const ServiceConfig& config);
    static ServiceConfig DeserializeServiceConfig(const std::vector<uint8_t>& data);

    static std::vector<uint8_t> SerializeProcessList(const std::vector<ProcessInfo>& processes);
    static std::vector<ProcessInfo> DeserializeProcessList(const std::vector<uint8_t>& data);

    static std::vector<uint8_t> SerializeRouteList(const std::vector<RouteInfo>& routes);
    static std::vector<RouteInfo> DeserializeRouteList(const std::vector<uint8_t>& data);

    static std::vector<uint8_t> SerializeStringList(const std::vector<std::string>& strings);
    static std::vector<std::string> DeserializeStringList(const std::vector<uint8_t>& data);
};