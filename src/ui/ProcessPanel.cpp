// src/ui/ProcessPanel.cpp
#include "ProcessPanel.h"
#include "ServiceClient.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include <algorithm>
#include <windowsx.h>
#include <commctrl.h>
#include <unordered_set>
#include <unordered_map>
#include <tlhelp32.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

ProcessPanel::ProcessPanel(HWND parent, ServiceClient* client)
    : parentWnd(parent), serviceClient(client), groupBox(nullptr), searchEdit(nullptr),
    listView(nullptr), isUpdating(false) {
}

ProcessPanel::~ProcessPanel() {
}

void ProcessPanel::Create(int x, int y, int width, int height) {
    CreateControls(x, y, width, height);

    if (serviceClient && serviceClient->IsConnected()) {
        auto config = serviceClient->GetConfig();
        selectedProcesses = config.selectedProcesses;

        Logger::Instance().Info("ProcessPanel::Create - Loaded " + std::to_string(selectedProcesses.size()) + " selected processes from config");
        for (const auto& proc : selectedProcesses) {
            Logger::Instance().Info("  - Selected process: " + proc);
        }
    }
    else {
        Logger::Instance().Warning("ProcessPanel::Create - Service not connected, cannot load config");
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

    SendMessage(searchEdit, EM_SETCUEBANNER, 0, (LPARAM)L"🔍 Search...");

    listView = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        x + 10, y + 55, width - 20, height - 70,
        parentWnd, (HMENU)3002, hInstance, nullptr);

    ListView_SetExtendedListViewStyle(listView,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMN column = { 0 };
    column.mask = LVCF_TEXT | LVCF_WIDTH;

    column.pszText = (LPWSTR)L"Process Name";
    column.cx = 250;
    ListView_InsertColumn(listView, 0, &column);

    column.pszText = (LPWSTR)L"Path";
    column.cx = width - 290;
    ListView_InsertColumn(listView, 1, &column);
}

void ProcessPanel::Refresh() {
    if (serviceClient && serviceClient->IsConnected()) {
        auto config = serviceClient->GetConfig();
        selectedProcesses = config.selectedProcesses;
        Logger::Instance().Info("ProcessPanel::Refresh - Reloaded selected processes: " + std::to_string(selectedProcesses.size()));
    }

    UpdateProcessList();
}

void ProcessPanel::UpdateProcessList() {
    isUpdating = true;

    Logger::Instance().Info("ProcessPanel::UpdateProcessList - Starting with " + std::to_string(selectedProcesses.size()) + " selected processes");

    std::unordered_map<std::wstring, ProcessDisplayInfo> uniqueProcessesMap;

    for (const auto& selectedName : selectedProcesses) {
        std::wstring wideName = Utils::StringToWString(selectedName);

        ProcessDisplayInfo info;
        info.name = wideName;
        info.path = L"(Not running)";
        info.isSelected = true;
        info.isRunning = false;

        uniqueProcessesMap[wideName] = info;
        Logger::Instance().Debug("Added selected process: " + selectedName);
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(snapshot, &pe32)) {
            do {
                std::wstring processName = pe32.szExeFile;
                std::string processNameStr = Utils::WStringToString(processName);

                if (processNameStr == "System" || processNameStr == "Registry" ||
                    processNameStr == "Idle" || processNameStr.empty() ||
                    processNameStr.find("svchost") != std::string::npos ||
                    processNameStr.find("RuntimeBroker") != std::string::npos ||
                    processNameStr.find("backgroundTask") != std::string::npos ||
                    processNameStr.find("conhost") != std::string::npos) {
                    continue;
                }

                bool isSelected = std::find_if(selectedProcesses.begin(), selectedProcesses.end(),
                    [&processNameStr](const std::string& selected) {
                        return _stricmp(processNameStr.c_str(), selected.c_str()) == 0;
                    }) != selectedProcesses.end();

                auto it = uniqueProcessesMap.find(processName);
                if (it != uniqueProcessesMap.end()) {
                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe32.th32ProcessID);
                    if (process) {
                        wchar_t path[MAX_PATH];
                        DWORD size = MAX_PATH;
                        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                            it->second.path = path;
                        }
                        CloseHandle(process);
                    }
                    it->second.isRunning = true;
                    it->second.isSelected = isSelected;
                }
                else {
                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe32.th32ProcessID);
                    if (!process) continue;

                    wchar_t path[MAX_PATH];
                    DWORD size = MAX_PATH;
                    bool hasPath = QueryFullProcessImageNameW(process, 0, path, &size);
                    CloseHandle(process);

                    ProcessDisplayInfo info;
                    info.name = processName;
                    info.path = hasPath ? path : L"";
                    info.isSelected = isSelected;
                    info.isRunning = true;

                    uniqueProcessesMap[processName] = info;
                }

            } while (Process32NextW(snapshot, &pe32));
        }

        CloseHandle(snapshot);
    }

    std::vector<ProcessDisplayInfo> displayProcesses;
    for (const auto& pair : uniqueProcessesMap) {
        displayProcesses.push_back(pair.second);
    }

    std::sort(displayProcesses.begin(), displayProcesses.end(),
        [](const ProcessDisplayInfo& a, const ProcessDisplayInfo& b) {
            if (a.isSelected && !b.isSelected) return true;
            if (!a.isSelected && b.isSelected) return false;

            std::string nameA = Utils::WStringToString(a.name);
            std::string nameB = Utils::WStringToString(b.name);

            if (Utils::IsDiscordProcess(nameA) && !Utils::IsDiscordProcess(nameB)) return true;
            if (!Utils::IsDiscordProcess(nameA) && Utils::IsDiscordProcess(nameB)) return false;
            if (Utils::IsGameProcess(nameA) && !Utils::IsGameProcess(nameB)) return true;
            if (!Utils::IsGameProcess(nameA) && Utils::IsGameProcess(nameB)) return false;

            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });

    SendMessage(listView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(listView);

    int checkedCount = 0;
    for (size_t i = 0; i < displayProcesses.size(); i++) {
        const auto& proc = displayProcesses[i];

        LVITEM item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(proc.name.c_str());

        int index = ListView_InsertItem(listView, &item);
        if (index == -1) continue;

        ListView_SetItemText(listView, index, 1, const_cast<LPWSTR>(proc.path.c_str()));

        ListView_SetCheckState(listView, index, proc.isSelected ? TRUE : FALSE);

        if (proc.isSelected) {
            checkedCount++;
            Logger::Instance().Debug("Checkbox ON for: " + Utils::WStringToString(proc.name));
        }
    }

    SendMessage(listView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(listView, NULL, TRUE);

    processes = displayProcesses;
    isUpdating = false;

    Logger::Instance().Info("ProcessPanel::UpdateProcessList completed - " +
        std::to_string(displayProcesses.size()) + " processes, " +
        std::to_string(checkedCount) + " checked");
}

void ProcessPanel::HandleCommand(WPARAM wParam) {
    WORD id = LOWORD(wParam);
    WORD notifyCode = HIWORD(wParam);

    if (id == 3001 && notifyCode == EN_CHANGE) {
        OnSearchChanged();
    }
}

void ProcessPanel::HandleNotify(LPNMHDR pnmh) {
    if (pnmh->idFrom == 3002 && pnmh->code == LVN_ITEMCHANGED) {
        LPNMLISTVIEW pnmlv = (LPNMLISTVIEW)pnmh;

        if (isUpdating) {
            return;
        }

        if (pnmlv->uChanged & LVIF_STATE) {
            UINT oldCheckState = (pnmlv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
            UINT newCheckState = (pnmlv->uNewState & LVIS_STATEIMAGEMASK) >> 12;

            if (oldCheckState != newCheckState && newCheckState != 0) {
                OnProcessToggle(pnmlv->iItem);
            }
        }
    }
}

void ProcessPanel::OnProcessToggle(int index) {
    if (index >= 0 && index < processes.size()) {
        bool checked = ListView_GetCheckState(listView, index);
        std::string processName = Utils::WStringToString(processes[index].name);

        Logger::Instance().Info("Process toggled: " + processName + " = " + (checked ? "ON" : "OFF"));

        if (checked) {
            if (std::find(selectedProcesses.begin(), selectedProcesses.end(), processName) == selectedProcesses.end()) {
                selectedProcesses.push_back(processName);
            }
        }
        else {
            selectedProcesses.erase(
                std::remove(selectedProcesses.begin(), selectedProcesses.end(), processName),
                selectedProcesses.end()
            );
        }

        if (serviceClient && serviceClient->IsConnected()) {
            serviceClient->SetSelectedProcesses(selectedProcesses);
            Logger::Instance().Info("Updated service with " + std::to_string(selectedProcesses.size()) + " selected processes");
        }
    }
}

void ProcessPanel::OnSearchChanged() {
    char searchText[256];
    GetWindowTextA(searchEdit, searchText, sizeof(searchText));
    FilterProcesses(searchText);
}

void ProcessPanel::FilterProcesses(const std::string& filter) {
    if (filter.empty()) {
        UpdateProcessList();
        return;
    }

    std::string lowerFilter = filter;
    std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);

    isUpdating = true;
    SendMessage(listView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(listView);

    int index = 0;
    for (size_t i = 0; i < processes.size(); i++) {
        std::string processName = Utils::WStringToString(processes[i].name);
        std::transform(processName.begin(), processName.end(), processName.begin(), ::tolower);

        if (processName.find(lowerFilter) != std::string::npos) {
            LVITEM item = { 0 };
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = const_cast<LPWSTR>(processes[i].name.c_str());

            int itemIndex = ListView_InsertItem(listView, &item);

            ListView_SetItemText(listView, itemIndex, 1, const_cast<LPWSTR>(processes[i].path.c_str()));
            ListView_SetCheckState(listView, itemIndex, processes[i].isSelected);
            index++;
        }
    }

    SendMessage(listView, WM_SETREDRAW, TRUE, 0);
    isUpdating = false;
}