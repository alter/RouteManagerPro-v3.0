// src/service/ProcessManager.cpp
#include "ProcessManager.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>

ProcessManager::ProcessManager(const ServiceConfig& config) : running(true) {
    Logger::Instance().Debug("ProcessManager::ProcessManager - Constructor called");

    // Загружаем выбранные процессы из конфигурации
    selectedProcesses.clear();
    selectedProcesses.insert(config.selectedProcesses.begin(), config.selectedProcesses.end());

    Logger::Instance().Info("ProcessManager initialized with " +
        std::to_string(selectedProcesses.size()) + " selected processes:");
    for (const auto& proc : selectedProcesses) {
        Logger::Instance().Info("  - " + proc);
    }

    UpdateProcessList();
    updateThread = std::thread(&ProcessManager::UpdateThreadFunc, this);
}

ProcessManager::~ProcessManager() {
    Logger::Instance().Debug("ProcessManager::~ProcessManager - Destructor called");
    running = false;

    if (updateThread.joinable()) {
        Logger::Instance().Debug("ProcessManager::~ProcessManager - Joining update thread");
        try {
            updateThread.join();
            Logger::Instance().Debug("ProcessManager::~ProcessManager - Update thread joined");
        }
        catch (const std::exception& e) {
            Logger::Instance().Error("ProcessManager::~ProcessManager - Exception joining thread: " + std::string(e.what()));
        }
    }
}

std::vector<ProcessInfo> ProcessManager::GetAllProcesses() const {
    std::lock_guard<std::mutex> lock(processesMutex);
    return allProcesses;
}

bool ProcessManager::IsProcessSelected(const std::string& processName) const {
    std::lock_guard<std::mutex> lock(processesMutex);

    for (const auto& selected : selectedProcesses) {
        if (selected.find('*') != std::string::npos) {
            if (MatchesWildcard(processName, selected)) {
                return true;
            }
        }
        else if (_stricmp(processName.c_str(), selected.c_str()) == 0) {
            return true;
        }
    }

    return false;
}

void ProcessManager::SetSelectedProcesses(const std::vector<std::string>& processes) {
    std::lock_guard<std::mutex> lock(processesMutex);
    selectedProcesses.clear();
    selectedProcesses.insert(processes.begin(), processes.end());
}

std::vector<std::string> ProcessManager::GetSelectedProcesses() const {
    std::lock_guard<std::mutex> lock(processesMutex);
    return std::vector<std::string>(selectedProcesses.begin(), selectedProcesses.end());
}

void ProcessManager::UpdateProcessList() {
    std::vector<ProcessInfo> newProcesses;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error("ProcessManager::UpdateProcessList - Failed to create snapshot");
        return;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == 0 || pe32.th32ProcessID == 4) continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe32.th32ProcessID);
            if (!process) continue;

            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                ProcessInfo info;
                info.name = pe32.szExeFile;
                info.executablePath = path;
                info.pid = pe32.th32ProcessID;

                std::string processName = Utils::WStringToString(info.name);
                info.isSelected = IsProcessSelected(processName);
                info.isGame = Utils::IsGameProcess(processName);
                info.isDiscord = Utils::IsDiscordProcess(processName);

                newProcesses.push_back(info);
            }

            CloseHandle(process);

        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);

    std::sort(newProcesses.begin(), newProcesses.end(),
        [](const ProcessInfo& a, const ProcessInfo& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });

    std::lock_guard<std::mutex> lock(processesMutex);
    allProcesses = std::move(newProcesses);
}

void ProcessManager::UpdateThreadFunc() {
    Logger::Instance().Debug("ProcessManager::UpdateThreadFunc - Started");

    while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
        // Interruptible sleep
        for (int i = 0; i < 50; i++) { // 5 seconds total
            if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
            break;
        }

        try {
            UpdateProcessList();
        }
        catch (const std::exception& e) {
            Logger::Instance().Error("ProcessManager::UpdateThreadFunc - Exception: " + std::string(e.what()));
        }
    }

    Logger::Instance().Debug("ProcessManager::UpdateThreadFunc - Exiting");
}

bool ProcessManager::MatchesWildcard(const std::string& processName, const std::string& pattern) const {
    std::string lowerProcess = processName;
    std::string lowerPattern = pattern;

    std::transform(lowerProcess.begin(), lowerProcess.end(), lowerProcess.begin(), ::tolower);
    std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(), ::tolower);

    size_t patternPos = 0;
    size_t processPos = 0;
    size_t starPos = std::string::npos;
    size_t matchPos = 0;

    while (processPos < lowerProcess.length()) {
        if (patternPos < lowerPattern.length() &&
            (lowerPattern[patternPos] == lowerProcess[processPos] || lowerPattern[patternPos] == '?')) {
            patternPos++;
            processPos++;
        }
        else if (patternPos < lowerPattern.length() && lowerPattern[patternPos] == '*') {
            starPos = patternPos++;
            matchPos = processPos;
        }
        else if (starPos != std::string::npos) {
            patternPos = starPos + 1;
            processPos = ++matchPos;
        }
        else {
            return false;
        }
    }

    while (patternPos < lowerPattern.length() && lowerPattern[patternPos] == '*') {
        patternPos++;
    }

    return patternPos == lowerPattern.length();
}