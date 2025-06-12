// src/ui/MainWindow.cpp
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <sstream>
#include <thread>

#include "MainWindow.h"
#include "ServiceClient.h"
#include "SystemTray.h"
#include "ProcessPanel.h"
#include "RouteTable.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"

#pragma comment(lib, "comctl32.lib")

MainWindow* MainWindow::instance = nullptr;

MainWindow::MainWindow(HINSTANCE hInst) : hwnd(nullptr), hInstance(hInst), isShuttingDown(false),
lastWidth(850), lastHeight(650) {
    instance = this;
}

MainWindow::~MainWindow() {
    instance = nullptr;
}

int MainWindow::Run(HINSTANCE hInstance, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    MainWindow window(hInstance);
    if (!window.CreateMainWindow(nCmdShow)) {
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}

bool MainWindow::CreateMainWindow(int nCmdShow) {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WindowProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"RouteManagerProWindow";
    wcex.hIconSm = wcex.hIcon;

    if (!RegisterClassExW(&wcex)) {
        return false;
    }

    hwnd = CreateWindowExW(
        0,
        L"RouteManagerProWindow",
        L"Route Manager Pro v3.0",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        850, 650,
        nullptr,
        nullptr,
        hInstance,
        this
    );

    if (!hwnd) {
        return false;
    }

    serviceClient = std::make_unique<ServiceClient>();

    // Connect to service with retry loop to avoid race condition
    Logger::Instance().Info("MainWindow: Connecting to service...");
    int retryCount = 0;
    const int maxRetries = 10;
    while (!serviceClient->Connect() && retryCount < maxRetries) {
        Logger::Instance().Debug("MainWindow: Connection attempt " + std::to_string(retryCount + 1) + " failed, retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retryCount++;
    }

    if (!serviceClient->IsConnected()) {
        Logger::Instance().Warning("MainWindow: Could not connect to service after " + std::to_string(maxRetries) + " attempts");
        MessageBox(hwnd, L"Warning: Could not connect to service.\nSome features may not work correctly.", L"Connection Warning", MB_OK | MB_ICONWARNING);
    }
    else {
        Logger::Instance().Info("MainWindow: Successfully connected to service");
    }

    systemTray = std::make_unique<SystemTray>(hwnd);
    processPanel = std::make_unique<ProcessPanel>(hwnd, serviceClient.get());
    routeTable = std::make_unique<RouteTable>(hwnd, serviceClient.get());

    CreateControls();

    LoadConfiguration();
    UpdateStatus();

    ShowWindow(hwnd, config.startMinimized ? SW_MINIMIZE : nCmdShow);
    UpdateWindow(hwnd);

    SetTimer(hwnd, 1, 1000, nullptr);
    SetTimer(hwnd, 2, 5000, nullptr);

    return true;
}

void MainWindow::CreateControls() {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    configGroupBox = CreateWindow(L"BUTTON", L"Configuration", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        10, 10, 200, 120, hwnd, nullptr, hInstance, nullptr);

    CreateWindow(L"STATIC", L"Gateway:", WS_CHILD | WS_VISIBLE,
        20, 35, 60, 20, hwnd, nullptr, hInstance, nullptr);

    gatewayEdit = CreateWindow(L"EDIT", L"10.200.210.1",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        85, 32, 115, 22, hwnd, nullptr, hInstance, nullptr);

    CreateWindow(L"STATIC", L"Metric:", WS_CHILD | WS_VISIBLE,
        20, 62, 60, 20, hwnd, nullptr, hInstance, nullptr);

    metricEdit = CreateWindow(L"EDIT", L"1",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        85, 59, 50, 22, hwnd, nullptr, hInstance, nullptr);

    applyButton = CreateWindow(L"BUTTON", L"Apply",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        140, 59, 60, 22, hwnd, (HMENU)1001, hInstance, nullptr);

    aiPreloadCheckbox = CreateWindow(L"BUTTON", L"Preload IPs",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        20, 90, 90, 20, hwnd, (HMENU)1002, hInstance, nullptr);

    editPreloadButton = CreateWindow(L"BUTTON", L"Edit Preload",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        115, 88, 85, 23, hwnd, (HMENU)1005, hInstance, nullptr);

    statusGroupBox = CreateWindow(L"BUTTON", L"Status", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        220, 10, 610, 120, hwnd, nullptr, hInstance, nullptr);

    statusLabel = CreateWindow(L"STATIC",
        L"Service: • Running\r\n"
        L"Monitor: • Active\r\n"
        L"Routes: 0 active\r\n"
        L"Memory: 0 MB\r\n"
        L"Uptime: 0m",
        WS_CHILD | WS_VISIBLE,
        230, 30, 590, 90, hwnd, nullptr, hInstance, nullptr);

    processPanel->Create(10, 140, 820, 240);
    routeTable->Create(10, 390, 820, 180);

    minimizeButton = CreateWindow(L"BUTTON", L"Minimize to Tray",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 580, 120, 30, hwnd, (HMENU)1003, hInstance, nullptr);

    viewLogsButton = CreateWindow(L"BUTTON", L"View Logs",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        140, 580, 100, 30, hwnd, (HMENU)1004, hInstance, nullptr);

    EnumChildWindows(hwnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        SendMessage(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE);
        return TRUE;
        }, (LPARAM)hFont);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001: instance->OnApplyConfig(); break;
        case 1002: instance->OnAIPreloadToggle(); break;
        case 1003: instance->OnMinimizeToTray(); break;
        case 1004: instance->OnViewLogs(); break;
        case 1005: instance->OnEditPreload(); break;
        default:
            if (instance->processPanel) {
                instance->processPanel->HandleCommand(wParam);
            }
            if (instance->routeTable) {
                instance->routeTable->HandleCommand(wParam);
            }
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == 1 && !instance->isShuttingDown) {
            instance->UpdateStatus();
        }
        else if (wParam == 2 && !instance->isShuttingDown) {
            if (instance->processPanel) {
                instance->processPanel->Refresh();
            }
            if (instance->routeTable) {
                instance->routeTable->Refresh();
            }
        }
        return 0;

    case Constants::WM_TRAY_ICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        else if (lParam == WM_RBUTTONUP) {
            instance->systemTray->ShowContextMenu();
        }
        return 0;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
        }
        else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
            instance->OnSize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_NOTIFY: {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (instance->processPanel) {
            instance->processPanel->HandleNotify(pnmh);
        }
        return 0;
    }

    case WM_USER + 100: // WM_ROUTES_CLEARED
        // Routes were cleared, refresh configuration and UI
        if (instance->serviceClient && instance->serviceClient->IsConnected()) {
            instance->config = instance->serviceClient->GetConfig();
            SendMessage(instance->aiPreloadCheckbox, BM_SETCHECK,
                instance->config.aiPreloadEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            Logger::Instance().Info("MainWindow: Updated AI preload checkbox after route cleanup");
        }
        return 0;

    case WM_CLOSE: {
        instance->OnClose();
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void MainWindow::OnEditPreload() {
    std::string configPath = Utils::GetCurrentDirectory() + "\\preload_ips.json";

    if (!Utils::FileExists(configPath)) {
        MessageBox(hwnd,
            L"Preload configuration file will be created.\n"
            L"You can edit it to add or remove IP ranges.",
            L"Information", MB_OK | MB_ICONINFORMATION);
    }

    std::wstring wideConfigPath = Utils::StringToWString(configPath);
    ShellExecuteW(nullptr, L"open", wideConfigPath.c_str(), nullptr, nullptr, SW_SHOW);
}

void MainWindow::OnSize(int width, int height) {
    if (width == 0 || height == 0) return;

    lastWidth = width;
    lastHeight = height;

    int statusWidth = width - 230;
    SetWindowPos(statusGroupBox, NULL, 220, 10, statusWidth, 120, SWP_NOZORDER);
    SetWindowPos(statusLabel, NULL, 230, 30, statusWidth - 10, 90, SWP_NOZORDER);

    int panelWidth = width - 20;
    if (processPanel) {
        processPanel->Resize(10, 140, panelWidth, 240);
    }

    if (routeTable) {
        routeTable->Resize(10, 390, panelWidth, height - 440);
    }

    SetWindowPos(minimizeButton, NULL, 10, height - 50, 120, 30, SWP_NOZORDER);
    SetWindowPos(viewLogsButton, NULL, 140, height - 50, 100, 30, SWP_NOZORDER);
}

void MainWindow::OnClose() {
    Logger::Instance().Info("MainWindow::OnClose - Starting shutdown sequence");

    if (isShuttingDown) {
        Logger::Instance().Warning("Already shutting down, ignoring duplicate close");
        return;
    }

    isShuttingDown = true;

    KillTimer(hwnd, 1);
    KillTimer(hwnd, 2);

    ShutdownCoordinator::Instance().InitiateShutdown();

    if (serviceClient && serviceClient->IsConnected()) {
        Logger::Instance().Info("Disconnecting from service");
        serviceClient->Disconnect();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (systemTray) {
        systemTray.reset();
    }

    processPanel.reset();
    routeTable.reset();
    serviceClient.reset();

    Logger::Instance().Info("Destroying main window");
    DestroyWindow(hwnd);
}

void MainWindow::UpdateStatus() {
    if (isShuttingDown) {
        return;
    }

    if (!serviceClient || !serviceClient->IsConnected()) {
        if (serviceClient) {
            serviceClient->Connect();
        }

        if (!serviceClient || !serviceClient->IsConnected()) {
            SetWindowText(statusLabel, L"Service: ○ Not Connected\r\nTrying to connect...");
            return;
        }
    }

    status = serviceClient->GetStatus();

    std::wstringstream ss;
    ss << L"Service: " << (status.isRunning ? L"●" : L"○") << L" Running\r\n";
    ss << L"Monitor: " << (status.monitorActive ? L"●" : L"○") << L" Active\r\n";
    ss << L"Routes: " << status.activeRoutes << L" active\r\n";
    ss << L"Memory: " << status.memoryUsageMB << L" MB\r\n";
    ss << L"Uptime: " << Utils::StringToWString(Utils::FormatDuration(status.uptime));

    SetWindowText(statusLabel, ss.str().c_str());
}

void MainWindow::OnApplyConfig() {
    char buffer[256];
    GetWindowTextA(gatewayEdit, buffer, sizeof(buffer));
    config.gatewayIp = buffer;

    GetWindowTextA(metricEdit, buffer, sizeof(buffer));
    config.metric = std::stoi(buffer);

    if (serviceClient && serviceClient->IsConnected()) {
        ServiceConfig currentConfig = serviceClient->GetConfig();
        config.selectedProcesses = currentConfig.selectedProcesses;
        config.startMinimized = currentConfig.startMinimized;
        config.startWithWindows = currentConfig.startWithWindows;
        config.aiPreloadEnabled = currentConfig.aiPreloadEnabled;
    }

    serviceClient->SetConfig(config);
}

void MainWindow::OnMinimizeToTray() {
    ShowWindow(hwnd, SW_HIDE);
}

void MainWindow::OnViewLogs() {
    std::string logPath = Utils::GetCurrentDirectory() + "\\logs";
    Utils::CreateDirectoryIfNotExists(logPath);
    std::wstring widePath = Utils::StringToWString(logPath);
    ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOW);
}

void MainWindow::OnAIPreloadToggle() {
    bool checked = SendMessage(aiPreloadCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (serviceClient && serviceClient->IsConnected()) {
        config = serviceClient->GetConfig();
    }

    config.aiPreloadEnabled = checked;
    serviceClient->SetAIPreload(checked);
}

void MainWindow::LoadConfiguration() {
    config = serviceClient->GetConfig();
    SetWindowTextA(gatewayEdit, config.gatewayIp.c_str());
    SetWindowTextA(metricEdit, std::to_string(config.metric).c_str());
    SendMessage(aiPreloadCheckbox, BM_SETCHECK, config.aiPreloadEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
}