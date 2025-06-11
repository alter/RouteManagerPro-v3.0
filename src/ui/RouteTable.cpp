// src/ui/RouteTable.cpp
#include "RouteTable.h"
#include "ServiceClient.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include <commctrl.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "comctl32.lib")

RouteTable::RouteTable(HWND parent, ServiceClient* client)
    : parentWnd(parent), listView(nullptr), cleanRoutesButton(nullptr), serviceClient(client) {
}

RouteTable::~RouteTable() {
}

void RouteTable::Create(int x, int y, int width, int height) {
    CreateControls(x, y, width, height);
    Refresh();
}

void RouteTable::CreateControls(int x, int y, int width, int height) {
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(parentWnd, GWLP_HINSTANCE);

    CreateWindow(L"BUTTON", L"Active Routes",
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

void RouteTable::UpdateRouteList() {
    size_t previousCount = routes.size();

    routes = serviceClient->GetRoutes();

    Logger::Instance().Debug("RouteTable::UpdateRouteList - Got " + std::to_string(routes.size()) + " routes");

    std::sort(routes.begin(), routes.end(),
        [](const RouteInfo& a, const RouteInfo& b) {
            return a.createdAt > b.createdAt;
        });

    SendMessage(listView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(listView);

    for (size_t i = 0; i < routes.size(); i++) {
        const RouteInfo& route = routes[i];

        LVITEM item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);

        std::wstring ip = Utils::StringToWString(route.ip);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(ip.c_str());
        int index = ListView_InsertItem(listView, &item);

        if (index != -1) {
            std::wstring process = Utils::StringToWString(route.processName);
            ListView_SetItemText(listView, index, 1, const_cast<LPWSTR>(process.c_str()));

            auto now = std::chrono::system_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - route.createdAt);

            wchar_t timeStr[64];
            if (duration.count() < 60) {
                swprintf_s(timeStr, L"Just now");
            }
            else if (duration.count() < 3600) {
                swprintf_s(timeStr, L"%lldm ago", duration.count() / 60);
            }
            else if (duration.count() < 86400) {
                swprintf_s(timeStr, L"%lldh %lldm ago", duration.count() / 3600, (duration.count() % 3600) / 60);
            }
            else {
                swprintf_s(timeStr, L"%lldd ago", duration.count() / 86400);
            }

            ListView_SetItemText(listView, index, 2, timeStr);

            wchar_t refs[32];
            swprintf_s(refs, L"%d", route.refCount.load());
            ListView_SetItemText(listView, index, 3, refs);
        }
    }

    SendMessage(listView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(listView, NULL, TRUE);

    if (routes.size() > previousCount && routes.size() > 0) {
        ListView_EnsureVisible(listView, 0, FALSE);
    }
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
        Refresh();
    }
}