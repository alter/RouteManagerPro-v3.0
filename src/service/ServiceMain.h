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

    static int Run();
    static int Install();
    static int Uninstall();

    void StartDirect();
    void StopDirect();

private:
    static ServiceMain* instance;
    static SERVICE_STATUS_HANDLE statusHandle;
    static SERVICE_STATUS serviceStatus;
    static HANDLE stopEvent;

    std::unique_ptr<NetworkMonitor> networkMonitor;
    std::unique_ptr<RouteController> routeController;
    std::unique_ptr<ProcessManager> processManager;
    std::unique_ptr<Watchdog> watchdog;
    std::unique_ptr<ConfigManager> configManager;

    std::atomic<bool> running;
    std::atomic<bool> isShuttingDown;
    std::atomic<bool> pipeServerRunning;
    HANDLE pipeThread;

    static VOID WINAPI ServiceMainEntry(DWORD argc, LPWSTR* argv);
    static VOID WINAPI ServiceCtrlHandler(DWORD ctrlCode);
    static DWORD WINAPI PipeServerThread(LPVOID param);

    void Start();
    void Stop();
    void ReportStatus(DWORD currentState, DWORD exitCode = NO_ERROR, DWORD waitHint = 0);
    void HandlePipeClient(HANDLE pipe);
};