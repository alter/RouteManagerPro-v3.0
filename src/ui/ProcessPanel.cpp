// src/ui/ProcessPanel.cpp
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#include "ProcessPanel.h"
#include "ServiceClient.h"
#include "../common/Utils.h"
#include "../common/Logger.h"

#pragma comment(lib, "psapi.lib")

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

ProcessPanel::ProcessPanel(HWND parent, ServiceClient* client)
    : parentWnd(parent), serviceClient(client), groupBox(nullptr), searchEdit(nullptr),
    availableListView(nullptr), selectedListView(nullptr), addButton(nullptr),
    removeButton(nullptr), addAllButton(nullptr), removeAllButton(nullptr),
    isUpdating(false), lastInteractionTime(0), isUserInteracting(false) {
}

ProcessPanel::~ProcessPanel() {
}

void ProcessPanel::Create(int x, int y, int width, int height) {
    CreateControls(x, y, width, height);

    if (serviceClient && serviceClient->IsConnected()) {
        auto config = serviceClient->GetConfig();
        selectedProcesses = config.selectedProcesses;

        Logger::Instance().Info("ProcessPanel::Create - Loaded " +
            std::to_string(selectedProcesses.size()) + " selected processes from config");
        for (const auto& proc : selectedProcesses) {
            Logger::Instance().Info("  - Selected process: " + proc);
        }
    }

    Refresh();
}

void ProcessPanel::CreateControls(int x, int y, int width, int height) {
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(parentWnd, GWLP_HINSTANCE);

    groupBox = CreateWindow(L"BUTTON", L"Process Selection",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, width, height, parentWnd, nullptr, hInstance, nullptr);

    searchEdit = CreateWindow(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        x + 10, y + 25, width - 20, 22,
        parentWnd, (HMENU)3001, hInstance, nullptr);

    SendMessage(searchEdit, EM_SETCUEBANNER, 0, (LPARAM)L"🔍 Search available processes...");

    // Calculate list dimensions
    int listWidth = (width - 90) / 2;  // 90 = margins + button area
    int listHeight = height - 70;

    // Available processes list
    availableListView = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        x + 10, y + 55, listWidth, listHeight,
        parentWnd, (HMENU)3002, hInstance, nullptr);

    ListView_SetExtendedListViewStyle(availableListView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Buttons
    int buttonX = x + 10 + listWidth + 10;
    int buttonY = y + 55 + (listHeight / 2) - 50;

    addButton = CreateWindow(L"BUTTON", L">",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        buttonX, buttonY, 30, 25,
        parentWnd, (HMENU)3003, hInstance, nullptr);

    removeButton = CreateWindow(L"BUTTON", L"<",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        buttonX, buttonY + 30, 30, 25,
        parentWnd, (HMENU)3004, hInstance, nullptr);

    addAllButton = CreateWindow(L"BUTTON", L">>",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        buttonX, buttonY + 60, 30, 25,
        parentWnd, (HMENU)3005, hInstance, nullptr);

    removeAllButton = CreateWindow(L"BUTTON", L"<<",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        buttonX, buttonY + 90, 30, 25,
        parentWnd, (HMENU)3006, hInstance, nullptr);

    // Selected processes list
    selectedListView = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        buttonX + 40, y + 55, listWidth, listHeight,
        parentWnd, (HMENU)3007, hInstance, nullptr);

    ListView_SetExtendedListViewStyle(selectedListView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Setup columns for both lists
    LVCOLUMN column = { 0 };
    column.mask = LVCF_TEXT | LVCF_WIDTH;

    // Available list columns
    column.pszText = (LPWSTR)L"Available Processes";
    column.cx = listWidth - 20;
    ListView_InsertColumn(availableListView, 0, &column);

    // Selected list columns
    column.pszText = (LPWSTR)L"Selected Processes";
    column.cx = listWidth - 20;
    ListView_InsertColumn(selectedListView, 0, &column);
}

void ProcessPanel::Refresh() {
    if (serviceClient && serviceClient->IsConnected()) {
        auto config = serviceClient->GetConfig();
        selectedProcesses = config.selectedProcesses;
        Logger::Instance().Info("ProcessPanel::Refresh - Reloaded selected processes: " +
            std::to_string(selectedProcesses.size()));
    }

    UpdateProcessList();
}

// Helper function to extract base process name from display name (removes suffixes like "(12)" or "(Not running)")
std::wstring ProcessPanel::GetBaseProcessNameFromDisplayName(const std::wstring& displayName) {
    // Remove "(Not running)" suffix if present
    size_t notRunningPos = displayName.find(L" (Not running)");
    if (notRunningPos != std::wstring::npos) {
        return displayName.substr(0, notRunningPos);
    }

    // Remove count suffix like "(12)" if present
    size_t lastSpace = displayName.find_last_of(L' ');
    if (lastSpace != std::wstring::npos &&
        displayName.length() > lastSpace + 1 &&
        displayName[lastSpace + 1] == L'(') {
        // Check if it ends with ')'
        if (displayName.back() == L')') {
            return displayName.substr(0, lastSpace);
        }
    }

    return displayName;
}

ProcessPanel::ScrollState ProcessPanel::SaveDetailedScrollPosition(HWND listView) {
    ScrollState state;

    // Get top visible item index
    state.topIndex = ListView_GetTopIndex(listView);

    // Calculate pixel offset for precise restoration
    if (state.topIndex >= 0) {
        RECT itemRect;
        if (ListView_GetItemRect(listView, state.topIndex, &itemRect, LVIR_BOUNDS)) {
            RECT clientRect;
            GetClientRect(listView, &clientRect);
            state.pixelOffset = itemRect.top - clientRect.top;
        }
    }

    // Save scrollbar info
    state.scrollInfo.cbSize = sizeof(SCROLLINFO);
    state.scrollInfo.fMask = SIF_ALL;
    GetScrollInfo(listView, SB_VERT, &state.scrollInfo);

    // Save selected item
    int selectedIndex = ListView_GetNextItem(listView, -1, LVNI_SELECTED);
    if (selectedIndex >= 0) {
        state.selectedItemName = GetBaseProcessNameFromDisplayName(GetItemText(listView, selectedIndex));
        state.hasSelection = true;
    }

    // Save focused item name as backup
    int focusedIndex = ListView_GetNextItem(listView, -1, LVNI_FOCUSED);
    if (focusedIndex >= 0) {
        state.focusedItemName = GetBaseProcessNameFromDisplayName(GetItemText(listView, focusedIndex));
    }

    Logger::Instance().Debug("SaveScrollPosition: topIndex=" + std::to_string(state.topIndex) +
        ", pixelOffset=" + std::to_string(state.pixelOffset));

    return state;
}

void ProcessPanel::RestoreDetailedScrollPosition(HWND listView, const ScrollState& state) {
    if (state.topIndex < 0 && state.selectedItemName.empty() && state.focusedItemName.empty()) {
        return; // Nothing to restore
    }

    int itemCount = ListView_GetItemCount(listView);
    if (itemCount == 0) {
        return; // Empty list
    }

    // Try to find the item that was at the top
    int targetIndex = -1;

    // First, try to restore by top index if valid
    if (state.topIndex >= 0 && state.topIndex < itemCount) {
        targetIndex = state.topIndex;
    }

    // If that fails, try to find by name
    if (targetIndex == -1 && !state.selectedItemName.empty()) {
        for (int i = 0; i < itemCount; i++) {
            std::wstring itemName = GetBaseProcessNameFromDisplayName(GetItemText(listView, i));
            if (itemName == state.selectedItemName) {
                targetIndex = i;
                break;
            }
        }
    }

    // Last resort - try focused item name
    if (targetIndex == -1 && !state.focusedItemName.empty()) {
        for (int i = 0; i < itemCount; i++) {
            std::wstring itemName = GetBaseProcessNameFromDisplayName(GetItemText(listView, i));
            if (itemName == state.focusedItemName) {
                targetIndex = i;
                break;
            }
        }
    }

    // Restore position
    if (targetIndex >= 0) {
        // First ensure the item is visible, without forcing it to the top
        ListView_EnsureVisible(listView, targetIndex, FALSE);

        // Then, calculate the precise scroll needed to match the saved pixel offset.
        RECT currentItemRect;
        if (ListView_GetItemRect(listView, targetIndex, &currentItemRect, LVIR_BOUNDS)) {
            // The amount to scroll is the difference between where the item is now
            // and where it's supposed to be.
            int scrollAmount = currentItemRect.top - state.pixelOffset;
            if (scrollAmount != 0) {
                ListView_Scroll(listView, 0, scrollAmount);
            }
        }

        // Restore selection if needed
        if (state.hasSelection && !state.selectedItemName.empty()) {
            for (int i = 0; i < itemCount; i++) {
                std::wstring itemName = GetBaseProcessNameFromDisplayName(GetItemText(listView, i));
                if (itemName == state.selectedItemName) {
                    ListView_SetItemState(listView, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    break;
                }
            }
        }

        Logger::Instance().Debug("RestoreScrollPosition: Restored to index " + std::to_string(targetIndex));
    }
}

bool ProcessPanel::IsUserInteracting() const {
    // Check if user interacted within last 2 seconds
    DWORD currentTime = GetTickCount();
    return (currentTime - lastInteractionTime) < 2000;
}

void ProcessPanel::OnUserInteraction() {
    lastInteractionTime = GetTickCount();
    isUserInteracting = true;
}

void ProcessPanel::UpdateProcessList() {
    // Skip update if user is actively interacting
    if (IsUserInteracting()) {
        Logger::Instance().Debug("UpdateProcessList: Skipped - user is interacting");
        return;
    }

    isUpdating = true;

    // Get current search filter
    char searchText[256] = "";
    GetWindowTextA(searchEdit, searchText, sizeof(searchText));
    std::string currentSearchFilter = searchText;
    std::transform(currentSearchFilter.begin(), currentSearchFilter.end(),
        currentSearchFilter.begin(), ::tolower);

    // Check if search changed
    bool searchChanged = (currentSearchFilter != lastSearchFilter);
    lastSearchFilter = currentSearchFilter;

    // Save scroll positions before update using the detailed method
    ScrollState availableScrollState;
    ScrollState selectedScrollState;
    if (!searchChanged) {
        availableScrollState = SaveDetailedScrollPosition(availableListView);
        selectedScrollState = SaveDetailedScrollPosition(selectedListView);
    }

    Logger::Instance().Info("ProcessPanel::UpdateProcessList - Starting with " +
        std::to_string(selectedProcesses.size()) + " selected processes");

    // Clear display vectors
    availableProcesses.clear();
    selectedProcessesDisplay.clear();

    // Build set of selected process names for quick lookup
    std::unordered_set<std::string> selectedSet;
    for (const auto& proc : selectedProcesses) {
        selectedSet.insert(proc);
    }

    // Track which selected processes are running
    std::unordered_map<std::string, ProcessDisplayInfo> runningSelected;

    // Use a map to store unique processes by name
    std::unordered_map<std::wstring, ProcessDisplayInfo> uniqueProcesses;

    // Get all running processes
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(snapshot, &pe32)) {
            do {
                std::wstring processName = pe32.szExeFile;
                std::string processNameStr = Utils::WStringToString(processName);

                // Skip system processes
                if (processNameStr == "System" || processNameStr == "Registry" ||
                    processNameStr == "Idle" || processNameStr.empty() ||
                    processNameStr.find("svchost") != std::string::npos ||
                    processNameStr.find("RuntimeBroker") != std::string::npos ||
                    processNameStr.find("backgroundTask") != std::string::npos ||
                    processNameStr.find("conhost") != std::string::npos) {
                    continue;
                }

                // If we haven't seen this process yet, add it
                if (uniqueProcesses.find(processName) == uniqueProcesses.end()) {
                    // Get process path
                    std::wstring processPath;
                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe32.th32ProcessID);
                    if (process) {
                        wchar_t path[MAX_PATH];
                        DWORD size = MAX_PATH;
                        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                            processPath = path;

                            // Skip Windows system processes
                            std::wstring pathLower = processPath;
                            std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);

                            if (pathLower.find(L"windows\\system32") != std::wstring::npos ||
                                pathLower.find(L"windows\\syswow64") != std::wstring::npos ||
                                pathLower.find(L"\\windowsapps\\") != std::wstring::npos) {
                                CloseHandle(process);
                                continue;
                            }
                        }
                        CloseHandle(process);
                    }

                    ProcessDisplayInfo info;
                    info.name = processName;
                    info.path = processPath;
                    info.isRunning = true;
                    info.isSelected = (selectedSet.find(processNameStr) != selectedSet.end());

                    uniqueProcesses[processName] = info;

                    if (info.isSelected) {
                        runningSelected[processNameStr] = info;
                    }
                }

            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }

    // Build available processes list (excluding selected ones)
    for (const auto& [name, info] : uniqueProcesses) {
        if (!info.isSelected) {
            // Apply search filter only to available processes
            if (!currentSearchFilter.empty()) {
                std::string nameLower = Utils::WStringToString(info.name);
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                if (nameLower.find(currentSearchFilter) == std::string::npos) {
                    continue;
                }
            }

            availableProcesses.push_back(info);
        }
    }

    // Add all selected processes (both running and not running)
    for (const auto& selectedName : selectedProcesses) {
        auto it = runningSelected.find(selectedName);
        if (it != runningSelected.end()) {
            selectedProcessesDisplay.push_back(it->second);
        }
        else {
            // Process not running
            ProcessDisplayInfo info;
            info.name = Utils::StringToWString(selectedName);
            info.path = L"(Not running)";
            info.isSelected = true;
            info.isRunning = false;
            selectedProcessesDisplay.push_back(info);
        }
    }

    // Sort both lists
    std::sort(availableProcesses.begin(), availableProcesses.end(),
        [](const ProcessDisplayInfo& a, const ProcessDisplayInfo& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });

    std::sort(selectedProcessesDisplay.begin(), selectedProcessesDisplay.end(),
        [](const ProcessDisplayInfo& a, const ProcessDisplayInfo& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });

    // Update available list
    // Lock window updates to prevent flickering and scrolling
    LockWindowUpdate(parentWnd);

    ListView_DeleteAllItems(availableListView);

    for (size_t i = 0; i < availableProcesses.size(); i++) {
        const auto& proc = availableProcesses[i];

        LVITEM item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(proc.name.c_str());

        ListView_InsertItem(availableListView, &item);
    }

    // Update selected list
    ListView_DeleteAllItems(selectedListView);

    for (size_t i = 0; i < selectedProcessesDisplay.size(); i++) {
        const auto& proc = selectedProcessesDisplay[i];

        LVITEM item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);

        std::wstring displayName = proc.name;
        if (!proc.isRunning) {
            displayName += L" (Not running)";
        }
        item.pszText = const_cast<LPWSTR>(displayName.c_str());

        ListView_InsertItem(selectedListView, &item);
    }

    // Unlock window updates
    LockWindowUpdate(NULL);

    // Update column headers with counts
    LVCOLUMN column = { 0 };
    column.mask = LVCF_TEXT;

    std::wstring availableHeader = L"Available Processes (" + std::to_wstring(availableProcesses.size()) + L")";
    column.pszText = const_cast<LPWSTR>(availableHeader.c_str());
    ListView_SetColumn(availableListView, 0, &column);

    std::wstring selectedHeader = L"Selected Processes (" + std::to_wstring(selectedProcessesDisplay.size()) + L")";
    column.pszText = const_cast<LPWSTR>(selectedHeader.c_str());
    ListView_SetColumn(selectedListView, 0, &column);

    // Restore scroll positions if search didn't change
    if (!searchChanged) {
        RestoreDetailedScrollPosition(availableListView, availableScrollState);
        RestoreDetailedScrollPosition(selectedListView, selectedScrollState);
    }

    isUpdating = false;

    Logger::Instance().Info("ProcessPanel::UpdateProcessList completed - Available: " +
        std::to_string(availableProcesses.size()) + ", Selected: " +
        std::to_string(selectedProcessesDisplay.size()));
}

void ProcessPanel::HandleCommand(WPARAM wParam) {
    WORD id = LOWORD(wParam);
    WORD notifyCode = HIWORD(wParam);

    switch (id) {
    case 3001: // Search edit
        if (notifyCode == EN_CHANGE) {
            OnSearchChanged();
        }
        break;
    case 3003: // Add button
        OnAddProcess();
        break;
    case 3004: // Remove button
        OnRemoveProcess();
        break;
    case 3005: // Add all button
        OnAddAllProcesses();
        break;
    case 3006: // Remove all button
        OnRemoveAllProcesses();
        break;
    }
}

void ProcessPanel::HandleNotify(LPNMHDR pnmh) {
    // Track user interaction for scrolling
    if (pnmh->code == LVN_BEGINSCROLL) {
        OnUserInteraction();
        Logger::Instance().Debug("User scrolling detected");
    }

    if (pnmh->code == NM_DBLCLK) {
        OnUserInteraction();
        if (pnmh->idFrom == 3002) { // Available list
            OnAddProcess();
        }
        else if (pnmh->idFrom == 3007) { // Selected list
            OnRemoveProcess();
        }
    }

    // Track item selection changes
    if (pnmh->code == LVN_ITEMCHANGED) {
        OnUserInteraction();
    }

    // Track mouse hover
    if (pnmh->code == NM_HOVER) {
        OnUserInteraction();
    }
}

int ProcessPanel::GetSelectedIndex(HWND listView) {
    return ListView_GetNextItem(listView, -1, LVNI_SELECTED);
}

std::wstring ProcessPanel::GetItemText(HWND listView, int index) {
    if (index < 0) return L"";

    wchar_t buffer[256] = { 0 };
    LVITEM item = { 0 };
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.pszText = buffer;
    item.cchTextMax = 256;

    if (ListView_GetItem(listView, &item)) {
        return std::wstring(buffer);
    }
    return L"";
}

void ProcessPanel::OnAddProcess() {
    int index = GetSelectedIndex(availableListView);
    if (index >= 0 && index < availableProcesses.size()) {
        std::string processName = Utils::WStringToString(availableProcesses[index].name);

        // Add to selected list
        if (std::find(selectedProcesses.begin(), selectedProcesses.end(), processName) == selectedProcesses.end()) {
            selectedProcesses.push_back(processName);

            // Update service
            if (serviceClient && serviceClient->IsConnected()) {
                serviceClient->SetSelectedProcesses(selectedProcesses);
                Logger::Instance().Info("Added process: " + processName);
            }

            // Refresh UI
            UpdateProcessList();
        }
    }
}

void ProcessPanel::OnRemoveProcess() {
    int index = GetSelectedIndex(selectedListView);
    if (index >= 0 && index < selectedProcessesDisplay.size()) {
        std::wstring processName = selectedProcessesDisplay[index].name;

        // Remove "(Not running)" suffix if present
        size_t pos = processName.find(L" (Not running)");
        if (pos != std::wstring::npos) {
            processName = processName.substr(0, pos);
        }

        std::string processNameStr = Utils::WStringToString(processName);

        // Remove from selected list
        auto it = std::find(selectedProcesses.begin(), selectedProcesses.end(), processNameStr);
        if (it != selectedProcesses.end()) {
            selectedProcesses.erase(it);

            // Update service
            if (serviceClient && serviceClient->IsConnected()) {
                serviceClient->SetSelectedProcesses(selectedProcesses);
                Logger::Instance().Info("Removed process: " + processNameStr);
            }

            // Refresh UI
            UpdateProcessList();
        }
    }
}

void ProcessPanel::OnAddAllProcesses() {
    for (const auto& proc : availableProcesses) {
        std::string processName = Utils::WStringToString(proc.name);
        if (std::find(selectedProcesses.begin(), selectedProcesses.end(), processName) == selectedProcesses.end()) {
            selectedProcesses.push_back(processName);
        }
    }

    if (serviceClient && serviceClient->IsConnected()) {
        serviceClient->SetSelectedProcesses(selectedProcesses);
        Logger::Instance().Info("Added all available processes");
    }

    UpdateProcessList();
}

void ProcessPanel::OnRemoveAllProcesses() {
    selectedProcesses.clear();

    if (serviceClient && serviceClient->IsConnected()) {
        serviceClient->SetSelectedProcesses(selectedProcesses);
        Logger::Instance().Info("Removed all selected processes");
    }

    UpdateProcessList();
}

void ProcessPanel::OnSearchChanged() {
    UpdateProcessList();
}

void ProcessPanel::Resize(int x, int y, int width, int height) {
    SetWindowPos(groupBox, NULL, x, y, width, height, SWP_NOZORDER);
    SetWindowPos(searchEdit, NULL, x + 10, y + 25, width - 20, 22, SWP_NOZORDER);

    int listWidth = (width - 90) / 2;
    int listHeight = height - 70;

    SetWindowPos(availableListView, NULL, x + 10, y + 55, listWidth, listHeight, SWP_NOZORDER);

    int buttonX = x + 10 + listWidth + 10;
    int buttonY = y + 55 + (listHeight / 2) - 50;

    SetWindowPos(addButton, NULL, buttonX, buttonY, 30, 25, SWP_NOZORDER);
    SetWindowPos(removeButton, NULL, buttonX, buttonY + 30, 30, 25, SWP_NOZORDER);
    SetWindowPos(addAllButton, NULL, buttonX, buttonY + 60, 30, 25, SWP_NOZORDER);
    SetWindowPos(removeAllButton, NULL, buttonX, buttonY + 90, 30, 25, SWP_NOZORDER);

    SetWindowPos(selectedListView, NULL, buttonX + 40, y + 55, listWidth, listHeight, SWP_NOZORDER);

    // Update column widths
    ListView_SetColumnWidth(availableListView, 0, listWidth - 20);
    ListView_SetColumnWidth(selectedListView, 0, listWidth - 20);
}