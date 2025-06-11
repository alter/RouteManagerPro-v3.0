// src/main.cpp
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "service/ServiceMain.h"
#include "ui/MainWindow.h"
#include "common/Logger.h"
#include "common/Utils.h"
#include "common/ShutdownCoordinator.h"

std::thread g_serviceThread;
std::atomic<bool> g_serviceRunning(false);
ServiceMain* g_service = nullptr;
std::mutex g_serviceMutex;
HANDLE g_shutdownEvent = nullptr;

void RunServiceInThread() {
    Logger::Instance().Info("RunServiceInThread - Starting service in background thread");
    g_serviceRunning = true;

    try {
        Logger::Instance().Debug("RunServiceInThread - Creating ServiceMain instance");
        g_service = new ServiceMain();

        Logger::Instance().Debug("RunServiceInThread - Calling StartDirect");
        g_service->StartDirect();

        Logger::Instance().Debug("RunServiceInThread - StartDirect returned, deleting service");
        delete g_service;
        g_service = nullptr;
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("RunServiceInThread - Exception: " + std::string(e.what()));
        if (g_service) {
            delete g_service;
            g_service = nullptr;
        }
    }
    catch (...) {
        Logger::Instance().Error("RunServiceInThread - Unknown exception");
        if (g_service) {
            delete g_service;
            g_service = nullptr;
        }
    }

    g_serviceRunning = false;
    Logger::Instance().Info("RunServiceInThread - Service thread ended");

    if (g_shutdownEvent) {
        Logger::Instance().Debug("RunServiceInThread - Setting shutdown event");
        SetEvent(g_shutdownEvent);
    }
}

void StopServiceThread() {
    Logger::Instance().Info("StopServiceThread - Called");

    std::lock_guard<std::mutex> lock(g_serviceMutex);

    if (!g_service) {
        Logger::Instance().Warning("StopServiceThread - No service to stop");
        return;
    }

    const auto maxShutdownTime = std::chrono::seconds(5);
    auto shutdownStart = std::chrono::steady_clock::now();

    try {
        Logger::Instance().Info("StopServiceThread - Calling StopDirect");
        g_service->StopDirect();

        auto elapsed = std::chrono::steady_clock::now() - shutdownStart;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            maxShutdownTime - elapsed);

        if (remaining.count() > 0) {
            Logger::Instance().Debug("StopServiceThread - Waiting for threads with timeout: " + std::to_string(remaining.count()) + "ms");
            ShutdownCoordinator::Instance().WaitForThreads(remaining);
        }

        if (g_shutdownEvent) {
            Logger::Instance().Debug("StopServiceThread - Waiting for shutdown event");
            DWORD result = WaitForSingleObject(g_shutdownEvent, 2000);
            if (result == WAIT_TIMEOUT) {
                Logger::Instance().Warning("StopServiceThread - Service thread did not complete within timeout");
            }
            else {
                Logger::Instance().Debug("StopServiceThread - Shutdown event signaled");
            }
        }

        if (g_serviceThread.joinable()) {
            Logger::Instance().Debug("StopServiceThread - Joining service thread");
            g_serviceThread.join();
            Logger::Instance().Debug("StopServiceThread - Service thread joined");
        }

    }
    catch (const std::exception& e) {
        Logger::Instance().Error("StopServiceThread - Exception during shutdown: " + std::string(e.what()));
    }
    catch (...) {
        Logger::Instance().Error("StopServiceThread - Unknown exception during shutdown");
    }

    Logger::Instance().Info("StopServiceThread - Completed");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    Logger::Instance().Info("WinMain - Application started");

    if (!Utils::IsRunAsAdmin()) {
        Logger::Instance().Warning("WinMain - Not running as admin");
        int result = MessageBoxW(NULL,
            L"Route Manager Pro requires administrator privileges to manage network routes.\n\n"
            L"Would you like to restart with administrator rights?",
            L"Administrator Rights Required",
            MB_YESNO | MB_ICONWARNING);

        if (result == IDYES) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);

            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpVerb = L"runas";
            sei.lpFile = path;
            sei.nShow = SW_NORMAL;

            if (!ShellExecuteExW(&sei)) {
                MessageBoxW(NULL, L"Failed to restart with administrator rights.", L"Error", MB_OK | MB_ICONERROR);
            }
        }
        return 1;
    }

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"RouteManagerPro");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Logger::Instance().Warning("WinMain - Another instance already running");
        MessageBoxW(NULL, L"Route Manager Pro is already running!", L"Error", MB_OK | MB_ICONWARNING);
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    Logger::Instance().Debug("WinMain - Creating shutdown event");
    g_shutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    int result = 0;

    try {
        Logger::Instance().Debug("WinMain - Starting service thread");
        g_serviceThread = std::thread(RunServiceInThread);

        // Give service time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        Logger::Instance().Debug("WinMain - Starting MainWindow");
        result = MainWindow::Run(hInstance, nCmdShow);
        Logger::Instance().Debug("WinMain - MainWindow returned with code: " + std::to_string(result));
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("WinMain - Exception: " + std::string(e.what()));
        result = 1;
    }
    catch (...) {
        Logger::Instance().Error("WinMain - Unknown exception");
        result = 1;
    }

    Logger::Instance().Info("WinMain - Stopping service before exit");
    StopServiceThread();

    if (g_shutdownEvent) {
        Logger::Instance().Debug("WinMain - Closing shutdown event");
        CloseHandle(g_shutdownEvent);
        g_shutdownEvent = nullptr;
    }

    if (mutex) {
        Logger::Instance().Debug("WinMain - Closing mutex");
        CloseHandle(mutex);
    }

    Logger::Instance().Info("WinMain - Application shutting down cleanly");
    return result;
}