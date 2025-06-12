// src/service/ServiceMain.cpp
#include <winsock2.h>
#include <windows.h>
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

// Static members for direct mode operation
HANDLE ServiceMain::stopEvent = nullptr;

ServiceMain::ServiceMain() : running(false), pipeThread(nullptr) {
    Logger::Instance().Debug("ServiceMain::ServiceMain() - Constructor called");
}

ServiceMain::~ServiceMain() {
    Logger::Instance().Debug("ServiceMain::~ServiceMain() - Destructor called");
    // StopDirect should be called explicitly by the owner before destruction
}

void ServiceMain::StartDirect() {
    Logger::Instance().Info("ServiceMain::StartDirect - Starting service logic");

    if (!stopEvent) {
        stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    }

    if (!stopEvent) {
        Logger::Instance().Error("ServiceMain::StartDirect - Failed to create stop event");
        return;
    }

    try {
        Logger::Instance().Debug("ServiceMain::StartDirect - Creating ConfigManager");
        configManager = std::make_unique<ConfigManager>();
        auto config = configManager->GetConfig();

        Logger::Instance().Debug("ServiceMain::StartDirect - Creating RouteController");
        routeController = std::make_unique<RouteController>(config);

        Logger::Instance().Debug("ServiceMain::StartDirect - Creating ProcessManager");
        processManager = std::make_unique<ProcessManager>(config);

        Logger::Instance().Debug("ServiceMain::StartDirect - Creating NetworkMonitor");
        networkMonitor = std::make_unique<NetworkMonitor>(
            routeController.get(), processManager.get(), nullptr);

        Logger::Instance().Debug("ServiceMain::StartDirect - Creating Watchdog");
        watchdog = std::make_unique<Watchdog>(this);

        Logger::Instance().Debug("ServiceMain::StartDirect - Starting NetworkMonitor");
        networkMonitor->Start();

        Logger::Instance().Debug("ServiceMain::StartDirect - Starting Watchdog");
        watchdog->Start();

        Logger::Instance().Debug("ServiceMain::StartDirect - Creating pipe server thread");
        pipeThread = CreateThread(nullptr, 0, PipeServerThread, this, 0, nullptr);

        running = true;
        Logger::Instance().Info("ServiceMain::StartDirect - Service logic is running");

        // Wait for the stop signal
        WaitForSingleObject(stopEvent, INFINITE);
        Logger::Instance().Debug("ServiceMain::StartDirect - Stop event signaled, exiting StartDirect()");
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("ServiceMain::StartDirect - Exception: " + std::string(e.what()));
        // Ensure graceful shutdown even on exception
        StopDirect();
        throw;
    }
}

void ServiceMain::StopDirect() {
    Logger::Instance().Info("ServiceMain::StopDirect - Starting graceful shutdown");

    static std::atomic<bool> stopInProgress(false);
    bool expected = false;
    if (!stopInProgress.compare_exchange_strong(expected, true)) {
        Logger::Instance().Warning("ServiceMain::StopDirect - Already in progress, returning");
        return;
    }

    if (!running.load()) {
        Logger::Instance().Warning("Service logic already stopped");
        stopInProgress = false;
        return;
    }

    Logger::Instance().Debug("ServiceMain::StopDirect - Setting running to false");
    running = false;

    Logger::Instance().Debug("ServiceMain::StopDirect - Initiating shutdown coordinator");
    ShutdownCoordinator::Instance().InitiateShutdown();

    // Signal the main loop in StartDirect to unblock
    if (stopEvent) {
        SetEvent(stopEvent);
    }

    try {
        if (networkMonitor) {
            Logger::Instance().Info("Stopping NetworkMonitor");
            networkMonitor->Stop();
            Logger::Instance().Debug("NetworkMonitor stopped successfully");
        }

        if (watchdog) {
            Logger::Instance().Info("Stopping Watchdog");
            watchdog->Stop();
            Logger::Instance().Debug("Watchdog stopped successfully");
        }

        if (pipeThread) {
            Logger::Instance().Info("Stopping pipe server thread");
            DWORD result = WaitForSingleObject(pipeThread, 5000);
            if (result == WAIT_TIMEOUT) {
                Logger::Instance().Warning("Pipe thread did not exit in time, terminating");
                TerminateThread(pipeThread, 1);
            }
            else {
                Logger::Instance().Debug("Pipe thread stopped successfully");
            }
            CloseHandle(pipeThread);
            pipeThread = nullptr;
        }

        Logger::Instance().Debug("ServiceMain::StopDirect - Resetting unique_ptrs");
        networkMonitor.reset();
        processManager.reset();
        routeController.reset();
        watchdog.reset();
        configManager.reset();

        if (stopEvent) {
            Logger::Instance().Debug("ServiceMain::StopDirect - Closing stop event handle");
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }

        Logger::Instance().Info("ServiceMain::StopDirect - Service logic stopped successfully");
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("ServiceMain::StopDirect - Exception during shutdown: " + std::string(e.what()));
    }
    catch (...) {
        Logger::Instance().Error("ServiceMain::StopDirect - Unknown exception during shutdown");
    }

    stopInProgress = false;
}

DWORD WINAPI ServiceMain::PipeServerThread(LPVOID param) {
    ServiceMain* service = static_cast<ServiceMain*>(param);
    Logger::Instance().Info("PipeServerThread: Starting pipe server");

    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    while (service->running && !ShutdownCoordinator::Instance().isShuttingDown) {
        Logger::Instance().Debug("PipeServerThread: Creating named pipe");

        HANDLE pipe = CreateNamedPipeA(
            Constants::PIPE_NAME.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096,
            4096,
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
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                DWORD bytesTransferred;
                if (GetOverlappedResult(pipe, &overlapped, &bytesTransferred, FALSE)) {
                    Logger::Instance().Debug("Client connected");
                    service->HandlePipeClient(pipe);
                }
            }
            else if (waitResult == WAIT_OBJECT_0 + 1) {
                Logger::Instance().Info("PipeServerThread: Shutdown requested");
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

    while (running && !ShutdownCoordinator::Instance().isShuttingDown) {
        DWORD bytesRead;
        std::vector<uint8_t> buffer(4096);

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
                auto config = configManager->GetConfig();
                response.data = IPCSerializer::SerializeServiceConfig(config);
                break;
            }

            case IPCMessageType::SetConfig: {
                auto config = IPCSerializer::DeserializeServiceConfig(msgData);
                configManager->SetConfig(config);
                break;
            }

            case IPCMessageType::GetProcesses: {
                auto processes = processManager->GetAllProcesses();
                response.data = IPCSerializer::SerializeProcessList(processes);
                break;
            }

            case IPCMessageType::SetSelectedProcesses: {
                auto processes = IPCSerializer::DeserializeStringList(msgData);
                processManager->SetSelectedProcesses(processes);
                auto config = configManager->GetConfig();
                config.selectedProcesses = processes;
                configManager->SetConfig(config);
                break;
            }

            case IPCMessageType::GetRoutes: {
                auto routes = routeController->GetActiveRoutes();
                response.data = IPCSerializer::SerializeRouteList(routes);
                break;
            }

            case IPCMessageType::ClearRoutes: {
                routeController->CleanupAllRoutes();
                break;
            }

            case IPCMessageType::SetAIPreload: {
                if (!msgData.empty()) {
                    bool enabled = msgData[0] != 0;
                    configManager->SetAIPreloadEnabled(enabled);
                    if (enabled && routeController) {
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