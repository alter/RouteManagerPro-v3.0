// src/service/ServiceMain.cpp
#include "ServiceMain.h"
#include "NetworkMonitor.h"
#include "RouteController.h"
#include "ProcessManager.h"
#include "Watchdog.h"
#include "ConfigManager.h"
#include "../common/Constants.h"
#include "../common/IPCProtocol.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <thread>

ServiceMain* ServiceMain::instance = nullptr;
SERVICE_STATUS_HANDLE ServiceMain::statusHandle = nullptr;
SERVICE_STATUS ServiceMain::serviceStatus = { 0 };
HANDLE ServiceMain::stopEvent = nullptr;

ServiceMain::ServiceMain() : running(false), pipeThread(nullptr), isShuttingDown(false),
pipeServerRunning(false) {
    Logger::Instance().Debug("ServiceMain::ServiceMain() - Constructor called");
    instance = this;
}

ServiceMain::~ServiceMain() {
    Logger::Instance().Debug("ServiceMain::~ServiceMain() - Destructor called");
    if (!isShuttingDown) {
        Stop();
    }
    instance = nullptr;
}

int ServiceMain::Run() {
    Logger::Instance().Info("ServiceMain::Run - Starting service control dispatcher");

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(Constants::SERVICE_NAME.c_str()), ServiceMainEntry },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD error = GetLastError();
        Logger::Instance().Error("ServiceMain::Run - StartServiceCtrlDispatcher failed: " + std::to_string(error));
        return error;
    }

    return 0;
}

int ServiceMain::Install() {
    SC_HANDLE scManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scManager) {
        return GetLastError();
    }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring servicePath = std::wstring(path) + L" --service";

    SC_HANDLE service = CreateServiceW(
        scManager,
        Constants::SERVICE_NAME.c_str(),
        Constants::SERVICE_DISPLAY_NAME.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        servicePath.c_str(),
        nullptr,
        nullptr,
        L"",
        nullptr,
        nullptr
    );

    DWORD error = 0;
    if (!service) {
        error = GetLastError();
    }
    else {
        SERVICE_DESCRIPTIONW desc;
        desc.lpDescription = const_cast<LPWSTR>(Constants::SERVICE_DESCRIPTION.c_str());
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &desc);

        SERVICE_FAILURE_ACTIONSW failureActions = { 0 };
        SC_ACTION actions[3] = {
            { SC_ACTION_RESTART, 60000 },
            { SC_ACTION_RESTART, 120000 },
            { SC_ACTION_NONE, 0 }
        };
        failureActions.dwResetPeriod = 86400;
        failureActions.lpRebootMsg = nullptr;
        failureActions.lpCommand = nullptr;
        failureActions.cActions = 3;
        failureActions.lpsaActions = actions;

        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions);

        CloseServiceHandle(service);
    }

    CloseServiceHandle(scManager);
    return error;
}

int ServiceMain::Uninstall() {
    SC_HANDLE scManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scManager) {
        return GetLastError();
    }

    SC_HANDLE service = OpenServiceW(scManager, Constants::SERVICE_NAME.c_str(), DELETE);
    DWORD error = 0;
    if (!service) {
        error = GetLastError();
    }
    else {
        if (!DeleteService(service)) {
            error = GetLastError();
        }
        CloseServiceHandle(service);
    }

    CloseServiceHandle(scManager);
    return error;
}

VOID WINAPI ServiceMain::ServiceMainEntry(DWORD argc, LPWSTR* argv) {
    statusHandle = RegisterServiceCtrlHandlerW(Constants::SERVICE_NAME.c_str(), ServiceCtrlHandler);
    if (!statusHandle) {
        return;
    }

    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(statusHandle, &serviceStatus);

    instance = new ServiceMain();
    instance->Start();

    serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(statusHandle, &serviceStatus);

    delete instance;
    instance = nullptr;
}

VOID WINAPI ServiceMain::ServiceCtrlHandler(DWORD ctrlCode) {
    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        if (instance) {
            instance->ReportStatus(SERVICE_STOP_PENDING);
            instance->Stop();
        }
        break;
    case SERVICE_CONTROL_INTERROGATE:
        break;
    default:
        break;
    }
}

void ServiceMain::StartDirect() {
    Logger::Instance().Info("ServiceMain::StartDirect - Starting service directly");

    if (!stopEvent) {
        stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        Logger::Instance().Debug("ServiceMain::StartDirect - Created stop event");
    }

    instance = this;
    Start();
}

void ServiceMain::StopDirect() {
    Logger::Instance().Info("ServiceMain::StopDirect - Stopping service directly");

    if (stopEvent) {
        Logger::Instance().Debug("ServiceMain::StopDirect - Setting stop event");
        SetEvent(stopEvent);
    }

    Stop();
}

void ServiceMain::Start() {
    Logger::Instance().Info("ServiceMain::Start - Starting service");

    if (!stopEvent) {
        stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    }

    if (!stopEvent) {
        ReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    try {
        Logger::Instance().Debug("ServiceMain::Start - Creating ConfigManager");
        configManager = std::make_unique<ConfigManager>();
        auto config = configManager->GetConfig();

        Logger::Instance().Debug("ServiceMain::Start - Creating RouteController");
        routeController = std::make_unique<RouteController>(config);

        Logger::Instance().Debug("ServiceMain::Start - Creating ProcessManager");
        processManager = std::make_unique<ProcessManager>(config);

        Logger::Instance().Debug("ServiceMain::Start - Creating NetworkMonitor");
        networkMonitor = std::make_unique<NetworkMonitor>(
            routeController.get(), processManager.get());

        Logger::Instance().Debug("ServiceMain::Start - Creating Watchdog");
        watchdog = std::make_unique<Watchdog>(this);

        Logger::Instance().Debug("ServiceMain::Start - Starting NetworkMonitor");
        networkMonitor->Start();

        Logger::Instance().Debug("ServiceMain::Start - Starting Watchdog");
        watchdog->Start();

        Logger::Instance().Debug("ServiceMain::Start - Creating pipe server thread");
        pipeServerRunning = true;
        pipeThread = CreateThread(nullptr, 0, PipeServerThread, this, 0, nullptr);

        running = true;
        ReportStatus(SERVICE_RUNNING);

        Logger::Instance().Info("ServiceMain::Start - Service is running");

        WaitForSingleObject(stopEvent, INFINITE);
        Logger::Instance().Debug("ServiceMain::Start - Stop event signaled, exiting Start()");
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("ServiceMain::Start - Exception: " + std::string(e.what()));
        throw;
    }
}

void ServiceMain::Stop() {
    Logger::Instance().Info("ServiceMain::Stop - Starting graceful shutdown");

    if (isShuttingDown.exchange(true)) {
        Logger::Instance().Warning("ServiceMain::Stop - Already shutting down");
        return;
    }

    if (!running) {
        Logger::Instance().Warning("Service already stopped");
        return;
    }

    Logger::Instance().Debug("ServiceMain::Stop - Setting running to false");
    running = false;
    pipeServerRunning = false;

    Logger::Instance().Debug("ServiceMain::Stop - Initiating shutdown coordinator");
    ShutdownCoordinator::Instance().InitiateShutdown();

    try {
        // Stop components in reverse order of creation
        if (watchdog) {
            Logger::Instance().Info("Stopping Watchdog");
            watchdog->Stop();
            Logger::Instance().Debug("Watchdog stopped successfully");
        }

        if (networkMonitor) {
            Logger::Instance().Info("Stopping NetworkMonitor");
            networkMonitor->Stop();
            Logger::Instance().Debug("NetworkMonitor stopped successfully");
        }

        // Cancel any pending IO operations on pipe
        if (pipeThread) {
            Logger::Instance().Info("Stopping pipe server thread");
            // Signal pipe thread to stop by closing any active connections
            DWORD result = WaitForSingleObject(pipeThread, 2000);
            if (result == WAIT_TIMEOUT) {
                Logger::Instance().Warning("Pipe thread did not exit in time");
                // Don't terminate thread, just continue
            }
            else {
                Logger::Instance().Debug("Pipe thread stopped successfully");
            }
            CloseHandle(pipeThread);
            pipeThread = nullptr;
        }

        if (stopEvent) {
            Logger::Instance().Debug("ServiceMain::Stop - Setting stop event");
            SetEvent(stopEvent);
        }

        // Clear components in reverse order
        Logger::Instance().Debug("ServiceMain::Stop - Clearing components");
        watchdog.reset();
        networkMonitor.reset();
        processManager.reset();
        routeController.reset();
        configManager.reset();

        if (stopEvent) {
            Logger::Instance().Debug("ServiceMain::Stop - Closing stop event handle");
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }

        Logger::Instance().Info("ServiceMain::Stop - Service stopped successfully");
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("ServiceMain::Stop - Exception during shutdown: " + std::string(e.what()));
    }
    catch (...) {
        Logger::Instance().Error("ServiceMain::Stop - Unknown exception during shutdown");
    }
}

void ServiceMain::ReportStatus(DWORD currentState, DWORD exitCode, DWORD waitHint) {
    static DWORD checkPoint = 1;

    Logger::Instance().Debug("ServiceMain::ReportStatus - State: " + std::to_string(currentState));

    serviceStatus.dwCurrentState = currentState;
    serviceStatus.dwWin32ExitCode = exitCode;
    serviceStatus.dwWaitHint = waitHint;

    if (currentState == SERVICE_START_PENDING) {
        serviceStatus.dwControlsAccepted = 0;
    }
    else {
        serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) {
        serviceStatus.dwCheckPoint = 0;
    }
    else {
        serviceStatus.dwCheckPoint = checkPoint++;
    }

    SetServiceStatus(statusHandle, &serviceStatus);
}

DWORD WINAPI ServiceMain::PipeServerThread(LPVOID param) {
    ServiceMain* service = static_cast<ServiceMain*>(param);
    Logger::Instance().Info("PipeServerThread: Starting pipe server");

    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    while (service->pipeServerRunning && !ShutdownCoordinator::Instance().isShuttingDown) {
        Logger::Instance().Debug("PipeServerThread: Creating named pipe");

        HANDLE pipe = CreateNamedPipeA(
            Constants::PIPE_NAME.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            nullptr
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            Logger::Instance().Error("PipeServerThread: Failed to create pipe: " + std::to_string(GetLastError()));
            Sleep(1000);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, &overlapped);
        if (!connected && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waitHandles[] = { overlapped.hEvent, ShutdownCoordinator::Instance().shutdownEvent };
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 5000);

            if (waitResult == WAIT_OBJECT_0) {
                DWORD bytesTransferred;
                if (GetOverlappedResult(pipe, &overlapped, &bytesTransferred, FALSE)) {
                    Logger::Instance().Debug("Client connected");
                    service->HandlePipeClient(pipe);
                }
            }
            else if (waitResult == WAIT_OBJECT_0 + 1 || waitResult == WAIT_TIMEOUT) {
                Logger::Instance().Info("PipeServerThread: Shutdown requested or timeout");
                CancelIo(pipe);
                CloseHandle(pipe);
                break;
            }
        }
        else if (connected) {
            service->HandlePipeClient(pipe);
        }

        CloseHandle(pipe);
    }

    CloseHandle(overlapped.hEvent);
    Logger::Instance().Info("PipeServerThread: Exiting");
    return 0;
}

void ServiceMain::HandlePipeClient(HANDLE pipe) {
    Logger::Instance().Debug("ServiceMain::HandlePipeClient - Starting");

    const size_t BUFFER_SIZE = 65536;

    while (running && !ShutdownCoordinator::Instance().isShuttingDown) {
        DWORD bytesRead;
        std::vector<uint8_t> buffer(BUFFER_SIZE);

        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
            DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                Logger::Instance().Debug("HandlePipeClient: ReadFile failed: " + std::to_string(error));
            }
            break;
        }

        if (bytesRead < sizeof(IPCMessageType)) {
            continue;
        }

        IPCMessageType msgType = *reinterpret_cast<IPCMessageType*>(buffer.data());
        std::vector<uint8_t> msgData(buffer.begin() + sizeof(IPCMessageType),
            buffer.begin() + bytesRead);

        IPCResponse response;
        response.success = true;

        try {
            switch (msgType) {
            case IPCMessageType::GetStatus: {
                ServiceStatus status;
                status.isRunning = running;
                status.monitorActive = networkMonitor && networkMonitor->IsActive();
                status.activeRoutes = routeController ? routeController->GetRouteCount() : 0;
                status.memoryUsageMB = watchdog ? watchdog->GetMemoryUsageMB() : 0;
                status.uptime = watchdog ? watchdog->GetUptime() : std::chrono::seconds(0);
                response.data = IPCSerializer::SerializeServiceStatus(status);
                break;
            }

            case IPCMessageType::GetConfig: {
                if (configManager) {
                    auto config = configManager->GetConfig();
                    response.data = IPCSerializer::SerializeServiceConfig(config);
                }
                break;
            }

            case IPCMessageType::SetConfig: {
                if (configManager) {
                    auto config = IPCSerializer::DeserializeServiceConfig(msgData);
                    configManager->SetConfig(config);
                }
                break;
            }

            case IPCMessageType::GetProcesses: {
                if (processManager) {
                    auto processes = processManager->GetAllProcesses();
                    response.data = IPCSerializer::SerializeProcessList(processes);
                }
                break;
            }

            case IPCMessageType::SetSelectedProcesses: {
                if (processManager && configManager) {
                    auto processes = IPCSerializer::DeserializeStringList(msgData);

                    Logger::Instance().Info("ServiceMain::HandlePipeClient - SetSelectedProcesses received " +
                        std::to_string(processes.size()) + " processes");

                    processManager->SetSelectedProcesses(processes);

                    auto config = configManager->GetConfig();
                    config.selectedProcesses = processes;
                    configManager->SetConfig(config);

                    Logger::Instance().Info("ServiceMain::HandlePipeClient - Configuration saved");
                }
                break;
            }

            case IPCMessageType::GetRoutes: {
                if (routeController) {
                    auto routes = routeController->GetActiveRoutes();
                    response.data = IPCSerializer::SerializeRouteList(routes);
                }
                break;
            }

            case IPCMessageType::ClearRoutes: {
                if (routeController) {
                    routeController->CleanupAllRoutes();
                }
                break;
            }

            case IPCMessageType::RestartService: {
                break;
            }

            case IPCMessageType::SetAIPreload: {
                if (!msgData.empty() && configManager && routeController) {
                    bool enabled = msgData[0] != 0;
                    configManager->SetAIPreloadEnabled(enabled);
                    if (enabled) {
                        routeController->PreloadAIRoutes();
                    }
                }
                break;
            }

            default:
                response.success = false;
                response.error = "Unknown message type";
                break;
            }
        }
        catch (const std::exception& e) {
            Logger::Instance().Error("HandlePipeClient: Exception processing message: " + std::string(e.what()));
            response.success = false;
            response.error = e.what();
        }

        std::vector<uint8_t> responseBuffer;
        responseBuffer.resize(sizeof(bool) + sizeof(size_t) + response.data.size() +
            sizeof(size_t) + response.error.size());

        size_t offset = 0;
        memcpy(responseBuffer.data() + offset, &response.success, sizeof(bool));
        offset += sizeof(bool);

        size_t dataSize = response.data.size();
        memcpy(responseBuffer.data() + offset, &dataSize, sizeof(size_t));
        offset += sizeof(size_t);

        if (dataSize > 0) {
            memcpy(responseBuffer.data() + offset, response.data.data(), dataSize);
            offset += dataSize;
        }

        size_t errorSize = response.error.size();
        memcpy(responseBuffer.data() + offset, &errorSize, sizeof(size_t));
        offset += sizeof(size_t);

        if (errorSize > 0) {
            memcpy(responseBuffer.data() + offset, response.error.data(), errorSize);
        }

        DWORD bytesWritten;
        WriteFile(pipe, responseBuffer.data(), static_cast<DWORD>(responseBuffer.size()),
            &bytesWritten, nullptr);
    }

    Logger::Instance().Debug("ServiceMain::HandlePipeClient - Exiting");
}