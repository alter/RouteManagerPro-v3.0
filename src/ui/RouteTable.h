// src/ui/RouteTable.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <vector>
#include "../common/Models.h"

class ServiceClient;

class RouteTable {
public:
    RouteTable(HWND parent, ServiceClient* client);
    ~RouteTable();

    void Create(int x, int y, int width, int height);
    void Refresh();
    void HandleCommand(WPARAM wParam);

private:
    HWND parentWnd;
    HWND listView;
    HWND cleanRoutesButton;
    ServiceClient* serviceClient;
    std::vector<RouteInfo> routes;

    void CreateControls(int x, int y, int width, int height);
    void UpdateRouteList();
    void OnCleanAllRoutes();
};