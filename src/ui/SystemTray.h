// src/ui/SystemTray.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <string>

#pragma comment(lib, "shell32.lib")

class SystemTray {
public:
    SystemTray(HWND parentWindow);
    ~SystemTray();

    void UpdateTooltip(const std::wstring& text);
    void ShowContextMenu();

private:
    HWND hwnd;
    NOTIFYICONDATAW nid;
    HMENU contextMenu;

    void CreateTrayIcon();
    void RemoveTrayIcon();
    void CreateContextMenu();
};