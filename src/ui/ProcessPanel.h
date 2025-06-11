// src/ui/ProcessPanel.h
#pragma once
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
    void Resize(int x, int y, int width, int height);

private:
    HWND parentWnd;
    HWND groupBox;
    HWND searchEdit;
    HWND listView;
    ServiceClient* serviceClient;
    std::vector<ProcessDisplayInfo> filteredProcesses;
    std::vector<std::string> selectedProcesses;
    bool isUpdating;
    int currentScrollPos;

    void CreateControls(int x, int y, int width, int height);
    void UpdateProcessList();
    void OnProcessToggle(int index);
    void OnSearchChanged();
    void FilterProcesses(const std::string& filter);
    void SaveScrollPosition();
    void RestoreScrollPosition();
};