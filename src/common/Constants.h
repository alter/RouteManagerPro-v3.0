// src/common/Constants.h
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <array>
#include <chrono>

namespace Constants {
    // File paths
    inline const std::string PIPE_NAME = "\\\\.\\pipe\\RouteManagerPro";
    inline const std::string CONFIG_FILE = "config.json";
    inline const std::string STATE_FILE = "state.json";
    inline const std::string LOG_FILE = "route_manager.log";
    inline const std::string PRELOAD_CONFIG_FILE = "preload_ips.json";

    // Limits
    const int MAX_ROUTES = 10000;
    const int MAX_MEMORY_MB = 500;
    const int MAX_CONNECTIONS = 10000;
    const int MAX_RETRY_ATTEMPTS = 10;

    // Timing intervals
    const int CLEANUP_INTERVAL_SEC = 300;
    const int WATCHDOG_INTERVAL_SEC = 10;
    const int ROUTE_VERIFY_INTERVAL_SEC = 30;
    const int PROCESS_UPDATE_INTERVAL_SEC = 2;
    const auto CONNECTION_RETRY_DELAY = std::chrono::milliseconds(100);
    const auto SAVE_INTERVAL = std::chrono::minutes(10);
    const auto UI_UPDATE_INTERVAL = std::chrono::seconds(1);
    const auto REFRESH_INTERVAL = std::chrono::seconds(5);

    // Port ranges
    const int DISCORD_MIN_PORT = 50000;
    const int DISCORD_MAX_PORT = 65535;

    // Queue sizes
    const int QUEUE_SIZE_DISCORD = 50;
    const int QUEUE_SIZE_GAMING = 100;
    const int QUEUE_SIZE_DEVELOPMENT = 200;
    const int QUEUE_SIZE_NORMAL = 500;

    // Windows messages
    const int WM_TRAY_ICON = WM_USER + 1;
    const int WM_ROUTES_CLEARED = WM_USER + 100;
    const int WM_ROUTE_COUNT_CHANGED = WM_USER + 101;

    // Cleanup thresholds
    const int CONNECTION_CLEANUP_HOURS = 1;
    const int ROUTE_CLEANUP_HOURS = 48;
    const auto AGGRESSIVE_CLEANUP_AGE = std::chrono::minutes(30);

    // Process filters - system processes to ignore
    inline const std::array<const char*, 8> SYSTEM_PROCESS_FILTERS = {
        "System", "Registry", "Idle", "svchost",
        "RuntimeBroker", "backgroundTask", "conhost", "dwm"
    };

    // Windows system paths to filter
    inline const std::array<const wchar_t*, 3> SYSTEM_PATH_FILTERS = {
        L"windows\\system32",
        L"windows\\syswow64",
        L"\\windowsapps\\"
    };

    // Game process indicators
    inline const std::array<const char*, 13> GAME_INDICATORS = {
        "game", "steam", "epic", "origin", "battle.net", "riot", "uplay",
        "huntgame", "squadgame", "cs2", "valorant", "overwatch", "wow"
    };

    // Development process indicators  
    inline const std::array<const char*, 12> DEV_INDICATORS = {
        "code", "devenv", "pycharm", "idea", "git", "docker", "cursor",
        "visual studio", "jetbrains", "sublime", "atom", "brackets"
    };

    // Discord process names
    inline const std::array<const char*, 3> DISCORD_PROCESSES = {
        "Discord.exe", "DiscordCanary.exe", "DiscordPTB.exe"
    };

    // Private IP ranges (as strings for validation)
    inline const std::array<const char*, 4> PRIVATE_IP_PREFIXES = {
        "10.", "192.168.", "172.", "127."
    };

    // Default configuration values
    inline const std::string DEFAULT_GATEWAY_IP = "10.200.210.1";
    const int DEFAULT_METRIC = 1;
    const bool DEFAULT_START_MINIMIZED = true;
    const bool DEFAULT_START_WITH_WINDOWS = true;
    const bool DEFAULT_AI_PRELOAD_ENABLED = false;

    // Window dimensions
    const int MAIN_WINDOW_WIDTH = 850;
    const int MAIN_WINDOW_HEIGHT = 650;
    const int BUTTON_WIDTH = 120;
    const int BUTTON_HEIGHT = 30;

    // List view column widths
    const int COLUMN_IP_WIDTH = 150;
    const int COLUMN_PROCESS_WIDTH = 200;
    const int COLUMN_TIME_WIDTH = 100;
    const int COLUMN_REFS_WIDTH = 50;

    // Network monitoring
    const int WINDIVERT_QUEUE_LENGTH = 16384;
    const int WINDIVERT_QUEUE_TIME = 1;
    const int WINDIVERT_QUEUE_SIZE = 8388608;

    // IPC buffer sizes
    const size_t IPC_INITIAL_BUFFER_SIZE = 65536;
    const size_t IPC_MAX_MESSAGE_SIZE = 1048576; // 1MB

    // Cache sizes
    const size_t MAIN_CACHE_MAX_SIZE = 10000;
    const size_t MISS_CACHE_MAX_SIZE = 1000;
    const size_t STRING_CACHE_MAX_SIZE = 5000;

    // Optimization thresholds
    const int MIN_HOSTS_TO_AGGREGATE = 2;
    const int OPTIMIZATION_INTERVAL_HOURS = 1;
    const float WASTE_THRESHOLD_30 = 0.75f;
    const float WASTE_THRESHOLD_29 = 0.80f;
    const float WASTE_THRESHOLD_28 = 0.85f;
    const float WASTE_THRESHOLD_27 = 0.90f;
    const float WASTE_THRESHOLD_26 = 0.90f;
    const float WASTE_THRESHOLD_25 = 0.92f;
    const float WASTE_THRESHOLD_24 = 0.95f;
}