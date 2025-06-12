// src/common/Constants.h
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>

// Removed service-specific constants
// const std::wstring SERVICE_NAME = L"RouteManagerPro";
// const std::wstring SERVICE_DISPLAY_NAME = L"Route Manager Pro Service";
// const std::wstring SERVICE_DESCRIPTION = L"Automatic VPN routing for selected applications";

namespace Constants {
    const std::string PIPE_NAME = "\\\\.\\pipe\\RouteManagerPro";
    const std::string CONFIG_FILE = "config.json";
    const std::string STATE_FILE = "state.json";
    const std::string LOG_FILE = "route_manager.log";

    const int MAX_ROUTES = 10000;
    const int MAX_MEMORY_MB = 500;
    const int CLEANUP_INTERVAL_SEC = 300;
    const int WATCHDOG_INTERVAL_SEC = 10;
    const int ROUTE_VERIFY_INTERVAL_SEC = 30;  // Проверка маршрутов каждые 30 секунд
    const int PROCESS_UPDATE_INTERVAL_SEC = 2; // Обновление списка процессов каждые 2 секунды

    const int DISCORD_MIN_PORT = 50000;
    const int DISCORD_MAX_PORT = 65535;

    const int QUEUE_SIZE_DISCORD = 50;
    const int QUEUE_SIZE_GAMING = 100;
    const int QUEUE_SIZE_DEVELOPMENT = 200;
    const int QUEUE_SIZE_NORMAL = 500;

    const int WM_TRAY_ICON = WM_USER + 1;
    // const int WM_SERVICE_STATUS = WM_USER + 2; // Removed

    // const int SERVICE_RESTART_MAX = 3; // Removed
    const int CONNECTION_CLEANUP_HOURS = 1;
    const int ROUTE_CLEANUP_HOURS = 48;
}
