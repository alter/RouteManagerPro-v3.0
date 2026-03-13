// src/service/StartupManager.h
#pragma once
#include <string>

class StartupManager {
public:
    static bool SetStartWithWindows(bool enabled);
    static bool IsStartWithWindowsEnabled();

private:
    static constexpr const wchar_t* TASK_NAME = L"RouteManagerPro";

    static std::wstring GetExecutablePath();
    static std::wstring GetExecutableDirectory();
};
