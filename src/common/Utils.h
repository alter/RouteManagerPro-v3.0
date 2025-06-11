// src/common/Utils.h
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <chrono>

class Utils {
public:
    static bool IsValidIPv4(const std::string& ip);
    static bool IsPrivateIP(const std::string& ip);
    static std::string WStringToString(const std::wstring& wstr);
    static std::wstring StringToWString(const std::string& str);
    static std::vector<std::string> SplitString(const std::string& str, char delimiter);
    static std::string GetLastError();
    static bool IsRunAsAdmin();
    static bool EnableDebugPrivilege();
    static std::string GetProcessNameFromPath(const std::string& path);
    static bool IsGameProcess(const std::string& processName);
    static bool IsDiscordProcess(const std::string& processName);
    static bool IsDevProcess(const std::string& processName);
    static std::string GetCurrentDirectory();
    static bool FileExists(const std::string& path);
    static bool CreateDirectoryIfNotExists(const std::string& path);
    static DWORD GetProcessIdByName(const std::wstring& processName);
    static std::string FormatBytes(size_t bytes);
    static std::string FormatDuration(std::chrono::seconds duration);
};