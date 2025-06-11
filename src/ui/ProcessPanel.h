// src/ui/ProcessPanel.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <memory>
#include "../common/Models.h"

class ServiceClient;

struct ProcessDisplayInfo {
    std::wstring name;
    std::wstring path;
    std::wstring type;
    bool isSelected;
    bool isRunning;
};

class ProcessPanel {
public:
    ProcessPanel(HWND parent, ServiceClient* client);
    ~ProcessPanel();

    void Create(int x, int y, int width, int height);
    void Refresh();
    void HandleCommand(WPARAM wParam);
    void HandleNotify(LPNMHDR pnmh);

private:
    HWND parentWnd;
    HWND groupBox;
    HWND searchEdit;
    HWND listView;
    ServiceClient* serviceClient;
    std::vector<ProcessDisplayInfo> processes;
    std::vector<std::string> selectedProcesses;
    bool isUpdating;

    void CreateControls(int x, int y, int width, int height);
    void UpdateProcessList();
    void OnProcessToggle(int index);
    void OnSearchChanged();
    void FilterProcesses(const std::string& filter);
};