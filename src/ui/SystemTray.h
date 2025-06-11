// src/ui/SystemTray.h
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>

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