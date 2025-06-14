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
    struct ScrollState {
        int topIndex;
        int pixelOffset;
        SCROLLINFO scrollInfo;
        std::wstring selectedItemName;
        std::wstring focusedItemName;
        bool hasSelection;

        ScrollState() : topIndex(-1), pixelOffset(0), hasSelection(false) {
            scrollInfo.cbSize = sizeof(SCROLLINFO);
        }
    };

    ProcessPanel(HWND parent, ServiceClient* client);
    ~ProcessPanel();

    void Create(int x, int y, int width, int height);
    void Refresh();
    void HandleCommand(WPARAM wParam);
    void HandleNotify(LPNMHDR pnmh);
    void Resize(int x, int y, int width, int height);

    void RestoreDetailedScrollPosition(HWND listView, const ScrollState& state);

private:
    HWND parentWnd;
    HWND groupBox;
    HWND searchEdit;
    HWND availableListView;
    HWND selectedListView;
    HWND addButton;
    HWND removeButton;
    HWND addAllButton;
    HWND removeAllButton;

    ServiceClient* serviceClient;

    std::vector<ProcessDisplayInfo> availableProcesses;
    std::vector<ProcessDisplayInfo> selectedProcessesDisplay;
    std::vector<std::string> selectedProcesses;

    bool isUpdating;

    DWORD lastInteractionTime;
    bool isUserInteracting;

    std::string lastSearchFilter;

    void CreateControls(int x, int y, int width, int height);
    void UpdateProcessList();
    void UpdateListsImmediately();
    void OnAddProcess();
    void OnRemoveProcess();
    void OnAddAllProcesses();
    void OnRemoveAllProcesses();
    void OnSearchChanged();
    void FilterProcesses(const std::string& filter);

    ScrollState SaveDetailedScrollPosition(HWND listView);

    bool IsUserInteracting() const;
    void OnUserInteraction();

    int GetSelectedIndex(HWND listView);
    std::wstring GetItemText(HWND listView, int index);

    std::wstring GetBaseProcessNameFromDisplayName(const std::wstring& displayName);
};