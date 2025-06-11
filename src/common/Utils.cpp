// src/common/Utils.cpp
#include "Utils.h"
#include <regex>
#include <sstream>
#include <iomanip>
#include <tlhelp32.h>
#include <windows.h>
#include <io.h>
#include <direct.h>

bool Utils::IsValidIPv4(const std::string& ip) {
    std::regex ipPattern("^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$");
    return std::regex_match(ip, ipPattern);
}

bool Utils::IsPrivateIP(const std::string& ip) {
    if (ip.substr(0, 3) == "10.") return true;
    if (ip.substr(0, 8) == "192.168.") return true;
    if (ip.substr(0, 4) == "172.") {
        int second = std::stoi(ip.substr(4, ip.find('.', 4) - 4));
        return second >= 16 && second <= 31;
    }
    if (ip.substr(0, 4) == "127.") return true;
    return false;
}

std::string Utils::WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &result[0], size, nullptr, nullptr);
    return result;
}

std::wstring Utils::StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &result[0], size);
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
    size_t size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buffer, 0, nullptr);

    std::wstring message(buffer, size);
    LocalFree(buffer);

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

    CloseHandle(token);
    return true;
}

std::string Utils::GetProcessNameFromPath(const std::string& path) {
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return path.substr(lastSlash + 1);
    }
    return path;
}

bool Utils::IsGameProcess(const std::string& processName) {
    std::string lower = processName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::vector<std::string> gameIndicators = {
        "game", "steam", "epic", "origin", "battle.net", "riot", "uplay",
        "huntgame", "squadgame", "cs2", "valorant", "overwatch", "wow"
    };

    for (const auto& indicator : gameIndicators) {
        if (lower.find(indicator) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool Utils::IsDiscordProcess(const std::string& processName) {
    std::string lower = processName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("discord") != std::string::npos;
}

bool Utils::IsDevProcess(const std::string& processName) {
    std::string lower = processName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::vector<std::string> devIndicators = {
        "code", "devenv", "pycharm", "idea", "git", "docker", "cursor",
        "visual studio", "jetbrains", "sublime", "atom", "brackets"
    };

    for (const auto& indicator : devIndicators) {
        if (lower.find(indicator) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string Utils::GetCurrentDirectory() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return path.substr(0, lastSlash);
    }
    return ".";
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
    const char* units[] = { "B", "KB", "MB", "GB" };
    int unit = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024 && unit < 3) {
        size /= 1024;
        unit++;
    }

    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return ss.str();
}

std::string Utils::FormatDuration(std::chrono::seconds duration) {
    auto days = duration.count() / 86400;
    auto hours = (duration.count() % 86400) / 3600;
    auto minutes = (duration.count() % 3600) / 60;

    std::stringstream ss;
    if (days > 0) ss << days << "d ";
    if (hours > 0) ss << hours << "h ";
    ss << minutes << "m";
    return ss.str();
}