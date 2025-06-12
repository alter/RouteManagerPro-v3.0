// src/main.cpp
#include <winsock2.h>
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

// Global service instance
ServiceMain* g_pServiceMain = nullptr;
std::thread g_serviceLogicThread;

void RunServiceLogic() {
    Logger::Instance().Info("RunServiceLogic - Starting service logic in background thread.");
    try {
        g_pServiceMain = new ServiceMain();
        g_pServiceMain->StartDirect(); // This blocks until StopDirect is called
        delete g_pServiceMain;
        g_pServiceMain = nullptr;
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("RunServiceLogic - Exception: " + std::string(e.what()));
        MessageBoxA(NULL, ("A critical error occurred in the background service: " + std::string(e.what())).c_str(), "Critical Error", MB_OK | MB_ICONERROR);
    }
    catch (...) {
        Logger::Instance().Error("RunServiceLogic - Unknown exception.");
        MessageBoxA(NULL, "An unknown critical error occurred in the background service.", "Critical Error", MB_OK | MB_ICONERROR);
    }
    Logger::Instance().Info("RunServiceLogic - Service logic thread has finished.");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
#ifdef NDEBUG
    Logger::Instance().SetLogLevel(Logger::LogLevel::LEVEL_INFO);
#else
    Logger::Instance().SetLogLevel(Logger::LogLevel::LEVEL_DEBUG);
#endif
    Logger::Instance().Info("WinMain - Application started.");

    // The application requires administrator privileges to manage network routes.
    if (!Utils::IsRunAsAdmin()) {
        Logger::Instance().Warning("WinMain - Not running as admin. Prompting for elevation.");
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

    // Ensure only one instance of the application is running.
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"RouteManagerProInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Logger::Instance().Warning("WinMain - Another instance is already running.");
        MessageBoxW(NULL, L"Route Manager Pro is already running!", L"Error", MB_OK | MB_ICONWARNING);
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    int result = 0;
    try {
        Logger::Instance().Debug("WinMain - Starting service logic thread.");
        g_serviceLogicThread = std::thread(RunServiceLogic);

        // Wait a bit for service to initialize
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        Logger::Instance().Debug("WinMain - Starting MainWindow.");
        result = MainWindow::Run(hInstance, nCmdShow);
        Logger::Instance().Debug("WinMain - MainWindow returned with code: " + std::to_string(result));
    }
    catch (const std::exception& e) {
        Logger::Instance().Error("WinMain - Exception during startup or UI run: " + std::string(e.what()));
        result = 1;
    }
    catch (...) {
        Logger::Instance().Error("WinMain - Unknown exception during startup or UI run.");
        result = 1;
    }

    // After the UI closes, we must signal the background thread to shut down.
    Logger::Instance().Info("WinMain - UI has closed. Initiating shutdown of service logic.");

    // Signal shutdown
    ShutdownCoordinator::Instance().InitiateShutdown();

    // Call StopDirect if service is still running
    if (g_pServiceMain) {
        Logger::Instance().Info("WinMain - Calling StopDirect on service");
        g_pServiceMain->StopDirect();
    }

    // Wait for the service logic thread to finish its cleanup and exit.
    if (g_serviceLogicThread.joinable()) {
        Logger::Instance().Debug("WinMain - Waiting for service logic thread.");
        try {
            // Just join without timeout - the service should exit quickly now
            g_serviceLogicThread.join();
            Logger::Instance().Debug("WinMain - Service logic thread joined successfully.");
        }
        catch (const std::exception& e) {
            Logger::Instance().Error("WinMain - Exception joining service thread: " + std::string(e.what()));
        }
    }

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    Logger::Instance().Info("WinMain - Application shutting down cleanly.");

    // Force flush logs
    Logger::Instance().Info("=== END OF LOG ===");

    return result;
}