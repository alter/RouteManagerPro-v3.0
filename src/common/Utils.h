// src/common/Utils.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <chrono>

class Utils {
public:
    // Inline для критически важных функций производительности
    static bool IsValidIPv4(const std::string& ip);
    static bool IsPrivateIP(const std::string& ip);

    // Супер-быстрое преобразование IP в uint32_t (inline)
    inline static uint32_t FastIPToUInt(const std::string& ip) {
        uint32_t addr = 0;
        int octets[4] = { 0 };
        int octetIndex = 0;
        int currentValue = 0;

        for (char c : ip) {
            if (c == '.') {
                if (octetIndex >= 4) return 0;
                octets[octetIndex++] = currentValue;
                currentValue = 0;
            }
            else if (c >= '0' && c <= '9') {
                currentValue = currentValue * 10 + (c - '0');
            }
        }
        if (octetIndex == 3) {
            octets[3] = currentValue;
        }

        return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    }

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