// src/common/Utils.cpp
#include "Utils.h"
#include <regex>
#include <format>
#include <tlhelp32.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <algorithm>
#include <ranges>
#include <sstream>

bool Utils::IsValidIPv4(const std::string& ip) {
    static const std::regex ipPattern(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)");
    return std::regex_match(ip, ipPattern);
}

bool Utils::IsPrivateIP(const std::string& ip) {
    if (ip.starts_with("10.")) return true;
    if (ip.starts_with("192.168.")) return true;
    if (ip.starts_with("172.")) {
        int second = std::stoi(ip.substr(4, ip.find('.', 4) - 4));
        return second >= 16 && second <= 31;
    }
    if (ip.starts_with("127.")) return true;
    return false;
}

std::string Utils::WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utils::StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), size);
    return result;
}

std::vector<std::string> Utils::SplitString(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;

    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}

std::string Utils::GetLastError() {
    DWORD error = ::GetLastError();
    if (error == 0) return std::string();

    LPWSTR buffer = nullptr;
    size_t size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    if (size == 0) {
        return std::format("Unknown error code: {}", error);
    }

    std::wstring message(buffer, size);
    LocalFree(buffer);

    // Remove trailing newlines
    message.erase(std::remove_if(message.begin(), message.end(),
        [](wchar_t c) { return c == L'\r' || c == L'\n'; }),
        message.end());

    return WStringToString(message);
}

bool Utils::IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    HANDLE token = nullptr;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            isAdmin = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }

    return isAdmin;
}

bool Utils::EnableDebugPrivilege() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid;
    if (!LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr)) {
        CloseHandle(token);
        return false;
    }

    bool result = (::GetLastError() == ERROR_SUCCESS);
    CloseHandle(token);
    return result;
}

std::string Utils::GetProcessNameFromPath(const std::string& path) {
    auto lastSlash = path.find_last_of("\\/");
    return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
}

bool Utils::IsGameProcess(const std::string& processName) {
    std::string lower = processName;
    std::ranges::transform(lower, lower.begin(), ::tolower);

    static constexpr std::array gameIndicators = {
        "game", "steam", "epic", "origin", "battle.net", "riot", "uplay",
        "huntgame", "squadgame", "cs2", "valorant", "overwatch", "wow"
    };

    return std::ranges::any_of(gameIndicators, [&lower](const auto& indicator) {
        return lower.contains(indicator);
        });
}

bool Utils::IsDiscordProcess(const std::string& processName) {
    std::string lower = processName;
    std::ranges::transform(lower, lower.begin(), ::tolower);
    return lower.contains("discord");
}

bool Utils::IsDevProcess(const std::string& processName) {
    std::string lower = processName;
    std::ranges::transform(lower, lower.begin(), ::tolower);

    static constexpr std::array devIndicators = {
        "code", "devenv", "pycharm", "idea", "git", "docker", "cursor",
        "visual studio", "jetbrains", "sublime", "atom", "brackets"
    };

    return std::ranges::any_of(devIndicators, [&lower](const auto& indicator) {
        return lower.contains(indicator);
        });
}

std::string Utils::GetCurrentDirectory() {
    char buffer[MAX_PATH];
    if (GetModuleFileNameA(nullptr, buffer, MAX_PATH) == 0) {
        return ".";
    }

    std::string path(buffer);
    auto lastSlash = path.find_last_of("\\/");
    return (lastSlash != std::string::npos) ? path.substr(0, lastSlash) : ".";
}

bool Utils::FileExists(const std::string& path) {
    return _access(path.c_str(), 0) == 0;
}

bool Utils::CreateDirectoryIfNotExists(const std::string& path) {
    if (_access(path.c_str(), 0) != 0) {
        return _mkdir(path.c_str()) == 0 || errno == EEXIST;
    }
    return true;
}

DWORD Utils::GetProcessIdByName(const std::wstring& processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (processName == pe32.szExeFile) {
                CloseHandle(snapshot);
                return pe32.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    return 0;
}

std::string Utils::FormatBytes(size_t bytes) {
    static constexpr std::array units = { "B", "KB", "MB", "GB" };
    int unit = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024 && unit < 3) {
        size /= 1024;
        unit++;
    }

    return std::format("{:.2f} {}", size, units[unit]);
}

std::string Utils::FormatDuration(std::chrono::seconds duration) {
    auto days = duration.count() / 86400;
    auto hours = (duration.count() % 86400) / 3600;
    auto minutes = (duration.count() % 3600) / 60;

    if (days > 0) {
        return std::format("{}d {}h {}m", days, hours, minutes);
    }
    else if (hours > 0) {
        return std::format("{}h {}m", hours, minutes);
    }
    else {
        return std::format("{}m", minutes);
    }
}