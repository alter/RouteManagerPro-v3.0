// src/ui/SystemTray.cpp
#include "SystemTray.h"
#include "../common/Constants.h"
#include "../common/resource.h"
#include "../common/Logger.h"

SystemTray::SystemTray(HWND parentWindow) : hwnd(parentWindow) {
    CreateContextMenu();
    CreateTrayIcon();
}

SystemTray::~SystemTray() {
    RemoveTrayIcon();
    if (contextMenu) {
        DestroyMenu(contextMenu);
    }
}

void SystemTray::CreateTrayIcon() {
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = Constants::WM_TRAY_ICON;

    // Load icon from resources
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAINICON));

    // If failed to load custom icon, try to load from exe
    if (!nid.hIcon) {
        // Try to extract icon from exe
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        ExtractIconExW(exePath, 0, &nid.hIcon, NULL, 1);

        if (!nid.hIcon) {
            // Last resort - use default application icon
            nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
            Logger::Instance().Warning("SystemTray: Failed to load custom icon, using default");
        }
    }
    else {
        Logger::Instance().Info("SystemTray: Successfully loaded custom icon");
    }

    wcscpy_s(nid.szTip, L"Route Manager Pro");

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        DWORD error = GetLastError();
        Logger::Instance().Error("SystemTray: Failed to add tray icon, error: " + std::to_string(error));
    }
    else {
        Logger::Instance().Info("SystemTray: Icon added successfully");
    }
}

void SystemTray::RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (nid.hIcon && nid.hIcon != LoadIcon(nullptr, IDI_APPLICATION)) {
        DestroyIcon(nid.hIcon);
    }
}

void SystemTray::UpdateTooltip(const std::wstring& text) {
    wcscpy_s(nid.szTip, text.c_str());
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void SystemTray::CreateContextMenu() {
    contextMenu = CreatePopupMenu();
    AppendMenu(contextMenu, MF_STRING, 2001, L"Show/Hide Window");
    AppendMenu(contextMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(contextMenu, MF_STRING, 2003, L"View Active Routes");
    AppendMenu(contextMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(contextMenu, MF_STRING, 2005, L"Exit");
}

void SystemTray::ShowContextMenu() {
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    int cmd = TrackPopupMenu(contextMenu, TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, nullptr);

    switch (cmd) {
    case 2001:
        ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
        break;
    case 2003:
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        break;
    case 2005:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        break;
    }
}