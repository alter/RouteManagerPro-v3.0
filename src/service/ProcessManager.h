#pragma once
// src/service/ProcessManager.h
#pragma once
#include <vector>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <atomic>
#include "../common/Models.h"

class ProcessManager {
public:
    ProcessManager(const ServiceConfig& config);
    ~ProcessManager();

    std::vector<ProcessInfo> GetAllProcesses() const;
    bool IsProcessSelected(const std::string& processName) const;
    void SetSelectedProcesses(const std::vector<std::string>& processes);
    std::vector<std::string> GetSelectedProcesses() const;

private:
    mutable std::mutex processesMutex;
    std::unordered_set<std::string> selectedProcesses;
    std::vector<ProcessInfo> allProcesses;
    std::atomic<bool> running;
    std::thread updateThread;

    void UpdateProcessList();
    void UpdateThreadFunc();
    bool MatchesWildcard(const std::string& processName, const std::string& pattern) const;
};