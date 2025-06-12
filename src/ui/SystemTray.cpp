// src/ui/SystemTray.cpp
#include "SystemTray.h"
#include "../common/Constants.h"

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
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Route Manager Pro");

    Shell_NotifyIconW(NIM_ADD, &nid);
}

void SystemTray::RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &nid);
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
    // AppendMenu(contextMenu, MF_STRING, 2004, L"Restart Service"); // Removed
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
        // This command ID should ideally be defined in a shared constants header for UI
        // Assuming 1004 is "View Logs" or similar. Let's make it more explicit.
        // For now, let's assume it should open the route table or main window.
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        break;
        // case 2004: // Removed
        //    SendMessage(hwnd, WM_COMMAND, 1007, 0);
        //    break;
    case 2005:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        break;
    }
}