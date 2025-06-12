// src/ui/RouteTable.h
#pragma once
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
    void Resize(int x, int y, int width, int height);

private:
    HWND parentWnd;
    HWND groupBox;
    HWND listView;
    HWND cleanRoutesButton;
    ServiceClient* serviceClient;
    std::vector<RouteInfo> routes;
    int currentScrollPos;

    void CreateControls(int x, int y, int width, int height);
    void UpdateRouteList();
    void OnCleanAllRoutes();
    void SaveScrollPosition();
    void RestoreScrollPosition();
};