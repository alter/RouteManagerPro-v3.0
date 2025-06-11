// src/ui/MainWindow.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include "../common/Models.h"

class ServiceClient;
class SystemTray;
class ProcessPanel;
class RouteTable;

class MainWindow {
public:
    static int Run(HINSTANCE hInstance, int nCmdShow);

private:
    static MainWindow* instance;
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd;
    HINSTANCE hInstance;

    std::unique_ptr<ServiceClient> serviceClient;
    std::unique_ptr<SystemTray> systemTray;
    std::unique_ptr<ProcessPanel> processPanel;
    std::unique_ptr<RouteTable> routeTable;

    HWND gatewayEdit;
    HWND metricEdit;
    HWND applyButton;
    HWND aiPreloadCheckbox;
    HWND statusLabel;
    HWND minimizeButton;
    HWND viewLogsButton;

    ServiceConfig config;
    ServiceStatus status;
    std::atomic<bool> isShuttingDown;

    MainWindow(HINSTANCE hInst);
    ~MainWindow();

    bool CreateMainWindow(int nCmdShow);
    void CreateControls();
    void UpdateStatus();
    void OnApplyConfig();
    void OnMinimizeToTray();
    void OnViewLogs();
    void OnAIPreloadToggle();
    void LoadConfiguration();
    void OnClose();
};