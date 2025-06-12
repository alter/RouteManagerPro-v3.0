// src/ui/RouteTable.cpp
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <sstream>
#include <iomanip>
#include <chrono>

#include "RouteTable.h"
#include "ServiceClient.h"
#include "../common/Utils.h"
#include "../common/Logger.h"

#pragma comment(lib, "comctl32.lib")

#define WM_ROUTES_CLEARED (WM_USER + 100)

RouteTable::RouteTable(HWND parent, ServiceClient* client)
    : parentWnd(parent), groupBox(nullptr), listView(nullptr), cleanRoutesButton(nullptr), serviceClient(client), currentScrollPos(-1) {
}

RouteTable::~RouteTable() {
}

void RouteTable::Create(int x, int y, int width, int height) {
    CreateControls(x, y, width, height);
    Refresh();
}

void RouteTable::CreateControls(int x, int y, int width, int height) {
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(parentWnd, GWLP_HINSTANCE);

    groupBox = CreateWindow(L"BUTTON", L"Active Routes",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, width, height, parentWnd, nullptr, hInstance, nullptr);

    listView = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        x + 10, y + 25, width - 20, height - 65,
        parentWnd, (HMENU)5001, hInstance, nullptr);

    ListView_SetExtendedListViewStyle(listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMN column;
    column.mask = LVCF_TEXT | LVCF_WIDTH;

    column.pszText = (LPWSTR)L"IP Address";
    column.cx = 150;
    ListView_InsertColumn(listView, 0, &column);

    column.pszText = (LPWSTR)L"Process";
    column.cx = 200;
    ListView_InsertColumn(listView, 1, &column);

    column.pszText = (LPWSTR)L"Created";
    column.cx = 100;
    ListView_InsertColumn(listView, 2, &column);

    column.pszText = (LPWSTR)L"Refs";
    column.cx = 50;
    ListView_InsertColumn(listView, 3, &column);

    cleanRoutesButton = CreateWindow(L"BUTTON", L"Clean All Routes",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 10, y + height - 35, 120, 25,
        parentWnd, (HMENU)5003, hInstance, nullptr);
}

void RouteTable::Refresh() {
    Logger::Instance().Debug("RouteTable::Refresh - Starting");
    UpdateRouteList();
}

void RouteTable::SaveScrollPosition() {
    SCROLLINFO si;
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    if (GetScrollInfo(listView, SB_VERT, &si)) {
        currentScrollPos = si.nPos;
    }
}

void RouteTable::RestoreScrollPosition() {
    if (currentScrollPos >= 0) {
        ListView_EnsureVisible(listView, currentScrollPos, FALSE);
        currentScrollPos = -1;
    }
}

void RouteTable::UpdateRouteList() {
    SaveScrollPosition();

    routes = serviceClient->GetRoutes();

    Logger::Instance().Debug("RouteTable::UpdateRouteList - Got " + std::to_string(routes.size()) + " routes");

    SendMessage(listView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(listView);

    for (size_t i = 0; i < routes.size(); i++) {
        const RouteInfo& route = routes[i];

        LVITEM item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);

        std::wstring ipDisplay = Utils::StringToWString(route.ip) + L"/" + std::to_wstring(route.prefixLength);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(ipDisplay.c_str());
        int index = ListView_InsertItem(listView, &item);

        if (index != -1) {
            std::wstring process = Utils::StringToWString(route.processName);
            ListView_SetItemText(listView, index, 1, const_cast<LPWSTR>(process.c_str()));

            auto now = std::chrono::system_clock::now();
            auto duration = now - route.createdAt;

            std::wstring timeStr;

            if (duration.count() < 0 || duration > std::chrono::hours(24 * 365 * 10)) {
                timeStr = L"Just now";
            }
            else {
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration).count();
                auto hours = std::chrono::duration_cast<std::chrono::hours>(duration).count();
                auto days = hours / 24;
                auto weeks = days / 7;
                auto months = days / 30;
                auto years = days / 365;

                if (seconds < 60) {
                    timeStr = L"Just now";
                }
                else if (minutes < 60) {
                    timeStr = std::to_wstring(minutes) + L"m ago";
                }
                else if (hours < 24) {
                    timeStr = std::to_wstring(hours) + L"h ago";
                }
                else if (days < 7) {
                    timeStr = std::to_wstring(days) + L"d ago";
                }
                else if (weeks < 4) {
                    timeStr = std::to_wstring(weeks) + L"w ago";
                }
                else if (months < 12) {
                    timeStr = std::to_wstring(months) + L"mo ago";
                }
                else {
                    timeStr = std::to_wstring(years) + L"y ago";
                }
            }

            ListView_SetItemText(listView, index, 2, const_cast<LPWSTR>(timeStr.c_str()));

            std::wstring refs = std::to_wstring(route.refCount.load());
            ListView_SetItemText(listView, index, 3, const_cast<LPWSTR>(refs.c_str()));
        }
    }

    SendMessage(listView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(listView, NULL, TRUE);

    RestoreScrollPosition();
}

void RouteTable::HandleCommand(WPARAM wParam) {
    WORD id = LOWORD(wParam);

    if (id == 5003) {
        OnCleanAllRoutes();
    }
}

void RouteTable::OnCleanAllRoutes() {
    int result = MessageBox(parentWnd,
        L"This will remove all routes created by Route Manager Pro.\n\n"
        L"Are you sure you want to continue?",
        L"Confirm Route Cleanup",
        MB_YESNO | MB_ICONQUESTION);

    if (result == IDYES) {
        serviceClient->ClearRoutes();
        MessageBox(parentWnd, L"All routes have been removed.", L"Success", MB_OK | MB_ICONINFORMATION);

        // Notify parent window that routes were cleared
        PostMessage(parentWnd, WM_ROUTES_CLEARED, 0, 0);

        Refresh();
    }
}

void RouteTable::Resize(int x, int y, int width, int height) {
    SetWindowPos(groupBox, NULL, x, y, width, height, SWP_NOZORDER);
    SetWindowPos(listView, NULL, x + 10, y + 25, width - 20, height - 65, SWP_NOZORDER);
    SetWindowPos(cleanRoutesButton, NULL, x + 10, y + height - 35, 120, 25, SWP_NOZORDER);

    LVCOLUMN column = { 0 };
    column.mask = LVCF_WIDTH;

    int totalWidth = width - 20 - GetSystemMetrics(SM_CXVSCROLL);
    column.cx = 150;
    ListView_SetColumn(listView, 0, &column);

    column.cx = totalWidth - 150 - 100 - 50;
    ListView_SetColumn(listView, 1, &column);

    column.cx = 100;
    ListView_SetColumn(listView, 2, &column);

    column.cx = 50;
    ListView_SetColumn(listView, 3, &column);
}