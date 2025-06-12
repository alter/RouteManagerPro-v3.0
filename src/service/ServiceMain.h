// src/service/ServiceMain.h
#pragma once
#include <windows.h>
#include <memory>
#include <atomic>

class NetworkMonitor;
class RouteController;
class ProcessManager;
class Watchdog;
class ConfigManager;

class ServiceMain {
public:
    ServiceMain();
    ~ServiceMain();

    void StartDirect();
    void StopDirect();

private:
    static HANDLE stopEvent;

    std::unique_ptr<NetworkMonitor> networkMonitor;
    std::unique_ptr<RouteController> routeController;
    std::unique_ptr<ProcessManager> processManager;
    std::unique_ptr<Watchdog> watchdog;
    std::unique_ptr<ConfigManager> configManager;

    std::atomic<bool> running;
    HANDLE pipeThread;

    static DWORD WINAPI PipeServerThread(LPVOID param);
    void HandlePipeClient(HANDLE pipe);
};