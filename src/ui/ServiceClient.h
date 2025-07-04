// src/ui/ServiceClient.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <vector>
#include <string>
#include "../common/Models.h"
#include "../common/IPCProtocol.h"

class ServiceClient {
public:
    ServiceClient();
    ~ServiceClient();

    bool IsConnected() const { return connected; }
    bool Connect();
    void Disconnect();

    ServiceStatus GetStatus();
    ServiceConfig GetConfig();
    void SetConfig(const ServiceConfig& config);
    std::vector<ProcessInfo> GetProcesses();
    void SetSelectedProcesses(const std::vector<std::string>& processes);
    std::vector<RouteInfo> GetRoutes();
    void ClearRoutes();
    void RestartService();
    void SetAIPreload(bool enabled);
    void OptimizeRoutes();
    void CleanupRedundantRoutes();

private:
    HANDLE pipe;
    bool connected;

    IPCResponse SendMessage(const IPCMessage& message);
};