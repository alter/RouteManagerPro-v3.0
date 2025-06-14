// src/service/ProcessManager.cpp
#include "ProcessManager.h"
#include "../common/Utils.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include "PerformanceMonitor.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <format>
#include <ranges>

ProcessManager::ProcessManager(const ServiceConfig& config, const PerformanceConfig& perfCfg)
    : running(true), perfConfig(perfCfg), m_pidMissCache(perfCfg.missCacheMaxSize),
    m_wstringToStringCache(perfCfg.stringCacheMaxSize),
    m_stringToWstringCache(perfCfg.stringCacheMaxSize) {

    Logger::Instance().Debug("ProcessManager::ProcessManager - Constructor called");

    selectedProcesses.clear();
    selectedProcesses.insert(config.selectedProcesses.begin(), config.selectedProcesses.end());

    Logger::Instance().Info(std::format("ProcessManager initialized with {} selected processes:",
        selectedProcesses.size()));
    for (const auto& proc : selectedProcesses) {
        Logger::Instance().Info(std::format("  - {}", proc));
    }

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
            Logger::Instance().Error(std::format("ProcessManager::~ProcessManager - Exception joining thread: {}", e.what()));
        }
    }
}

bool ProcessManager::IsSelectedProcessByPid(DWORD pid) {
    PERF_TIMER("ProcessManager::IsSelectedProcessByPid");

    auto cachedInfo = GetCachedInfo(pid);
    if (cachedInfo.has_value()) {
        stats.hits.fetch_add(1);
        return cachedInfo->isSelected;
    }

    auto info = GetCompleteProcessInfo(pid);
    if (info.has_value()) {
        m_pidMissCache.put(pid, *info);
        stats.misses.fetch_add(1);
        stats.newProcessChecks.fetch_add(1);
        return info->isSelected;
    }

    stats.misses.fetch_add(1);
    return false;
}

std::optional<CachedProcessInfo> ProcessManager::GetCachedInfo(DWORD pid) {
    {
        std::shared_lock lock(cachesMutex);

        if (auto it = m_pidCache.find(pid); it != m_pidCache.end()) {
            stats.hits.fetch_add(1);
            return it->second;
        }
    }

    auto missInfo = m_pidMissCache.get(pid);
    if (missInfo.has_value()) {
        stats.hits.fetch_add(1);
        return missInfo;
    }

    stats.misses.fetch_add(1);
    return std::nullopt;
}

std::optional<CachedProcessInfo> ProcessManager::CheckProcessAndCache(DWORD pid) {
    auto existing = GetCachedInfo(pid);
    if (existing.has_value()) {
        return existing;
    }

    stats.newProcessChecks.fetch_add(1);

    auto info = GetCompleteProcessInfo(pid);
    if (info.has_value() && info->isSelected) {
        m_pidMissCache.put(pid, *info);
        return info;
    }

    return std::nullopt;
}

void ProcessManager::UpdateVerificationTime(DWORD pid) {
    std::unique_lock lock(cachesMutex);

    if (auto it = m_pidCache.find(pid); it != m_pidCache.end()) {
        it->second.lastVerified = std::chrono::steady_clock::now();
        stats.verificationChecks.fetch_add(1);
    }
}

void ProcessManager::AddToPidCache(DWORD pid, const CachedProcessInfo& info) {
    std::unique_lock lock(cachesMutex);

    if (m_pidCache.size() >= perfConfig.mainCacheMaxSize) {
        Logger::Instance().Warning(std::format("Main cache full, not adding PID {}", pid));
        return;
    }

    m_pidCache[pid] = info;
}

std::vector<ProcessInfo> ProcessManager::GetAllProcesses() const {
    std::shared_lock lock(cachesMutex);
    return allProcesses;
}

bool ProcessManager::IsProcessSelected(const std::string& processName) const {
    return IsProcessSelectedInternal(processName);
}

bool ProcessManager::IsProcessSelectedInternal(const std::string& processName) const {
    std::lock_guard<std::mutex> lock(selectedMutex);

    return std::ranges::any_of(selectedProcesses, [&processName, this](const auto& selected) {
        if (selected.contains('*')) {
            return MatchesWildcard(processName, selected);
        }
        return _stricmp(processName.c_str(), selected.c_str()) == 0;
        });
}

void ProcessManager::SetSelectedProcesses(const std::vector<std::string>& processes) {
    {
        std::lock_guard<std::mutex> lock(selectedMutex);
        selectedProcesses.clear();
        selectedProcesses.insert(processes.begin(), processes.end());
    }

    {
        std::unique_lock lock(cachesMutex);
        m_pidCache.clear();
        m_pidMissCache.clear();
    }
}

std::vector<std::string> ProcessManager::GetSelectedProcesses() const {
    std::lock_guard<std::mutex> lock(selectedMutex);
    return std::vector<std::string>(selectedProcesses.begin(), selectedProcesses.end());
}

void ProcessManager::UpdateThreadFunc() {
    Logger::Instance().Debug("ProcessManager::UpdateThreadFunc - Started");

    while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
        for (int i = 0; i < 50; i++) {
            if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!running.load() || ShutdownCoordinator::Instance().isShuttingDown) {
            break;
        }

        try {
            PERF_TIMER("ProcessManager::UpdateSnapshot");

            auto newCache = BuildProcessSnapshot();

            MergeMissCacheIntoMain(newCache);

            {
                std::unique_lock lock(cachesMutex);
                m_pidCache = std::move(newCache);
                m_pidMissCache.clear();

                allProcesses.clear();
                for (const auto& [pid, info] : m_pidCache) {
                    ProcessInfo procInfo;
                    procInfo.name = info.name;
                    procInfo.executablePath = CachedStringToWString(info.processPath);
                    procInfo.pid = pid;
                    procInfo.isSelected = info.isSelected;
                    procInfo.isGame = Utils::IsGameProcess(CachedWStringToString(info.name));
                    procInfo.isDiscord = Utils::IsDiscordProcess(CachedWStringToString(info.name));
                    allProcesses.push_back(procInfo);
                }
            }

            LogPerformanceStats();
        }
        catch (const std::exception& e) {
            Logger::Instance().Error(std::format("ProcessManager::UpdateThreadFunc - Exception: {}", e.what()));
        }
    }

    Logger::Instance().Debug("ProcessManager::UpdateThreadFunc - Exiting");
}

std::unordered_map<DWORD, CachedProcessInfo> ProcessManager::BuildProcessSnapshot() {
    PERF_TIMER("ProcessManager::BuildProcessSnapshot");

    std::unordered_map<DWORD, CachedProcessInfo> newCache;
    std::unordered_set<DWORD> alivePids;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error("ProcessManager::BuildProcessSnapshot - Failed to create snapshot");
        return newCache;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == 0 || pe32.th32ProcessID == 4) continue;

            alivePids.insert(pe32.th32ProcessID);

            auto infoOpt = GetCompleteProcessInfo(pe32.th32ProcessID);
            if (infoOpt.has_value()) {
                newCache[pe32.th32ProcessID] = *infoOpt;
            }

        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);

    return newCache;
}

std::optional<CachedProcessInfo> ProcessManager::GetCompleteProcessInfo(DWORD pid) {
    PERF_TIMER("ProcessManager::GetCompleteProcessInfo");

    UniqueHandle process(OpenProcess(PROCESS_QUERY_INFORMATION |
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid), HandleDeleter{});
    if (!process) return std::nullopt;

    CachedProcessInfo info;
    FILETIME creationTime, exitTime, kernelTime, userTime;

    if (!GetProcessTimes(process.get(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        return std::nullopt;
    }

    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
        return std::nullopt;
    }

    info.creationTime = creationTime;
    info.processPath = CachedWStringToString(path);
    info.name = CachedStringToWString(Utils::GetProcessNameFromPath(info.processPath));
    info.isSelected = IsProcessSelectedInternal(CachedWStringToString(info.name));
    info.lastVerified = std::chrono::steady_clock::now();

    return info;
}

void ProcessManager::MergeMissCacheIntoMain(std::unordered_map<DWORD, CachedProcessInfo>& mainCache) {
    m_pidMissCache.forEach([&](DWORD pid, const CachedProcessInfo& cachedInfo) {
        if (!mainCache.contains(pid)) {
            auto currentInfo = GetCompleteProcessInfo(pid);
            if (currentInfo.has_value()) {
                mainCache[pid] = *currentInfo;
            }
        }
        });
}

void ProcessManager::GetCacheStats(uint64_t& hits, uint64_t& misses) const {
    hits = stats.hits.load();
    misses = stats.misses.load();
}

void ProcessManager::LogPerformanceStats() const {
    auto hits = stats.hits.load();
    auto misses = stats.misses.load();
    auto verifications = stats.verificationChecks.load();
    auto newChecks = stats.newProcessChecks.load();
    auto stringHits = stats.stringCacheHits.load();
    auto stringMisses = stats.stringCacheMisses.load();

    if (hits + misses == 0) return;

    double hitRate = hits / static_cast<double>(hits + misses) * 100;
    double stringHitRate = stringHits / static_cast<double>(stringHits + stringMisses + 1) * 100;

    Logger::Instance().Info(std::format(
        "ProcessManager Cache: {} hits, {} misses ({:.1f}% hit rate), {} verifications, {} new process checks",
        hits, misses, hitRate, verifications, newChecks
    ));

    Logger::Instance().Info(std::format(
        "String Cache: {} hits, {} misses ({:.1f}% hit rate)",
        stringHits, stringMisses, stringHitRate
    ));
}

bool ProcessManager::MatchesWildcard(const std::string& processName, const std::string& pattern) const {
    std::string lowerProcess = processName;
    std::string lowerPattern = pattern;

    std::ranges::transform(lowerProcess, lowerProcess.begin(), ::tolower);
    std::ranges::transform(lowerPattern, lowerPattern.begin(), ::tolower);

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

void ProcessManager::CleanupStalePids(std::unordered_map<DWORD, CachedProcessInfo>& cache,
    const std::unordered_set<DWORD>& alivePids) {
    std::erase_if(cache, [&alivePids, this](const auto& pair) {
        bool shouldErase = !alivePids.contains(pair.first);
        if (shouldErase) {
            stats.cacheEvictions.fetch_add(1);
        }
        return shouldErase;
        });
}

std::string ProcessManager::CachedWStringToString(const std::wstring& wstr) {
    PERF_TIMER("ProcessManager::StringConversion");

    // Check cache first
    auto cached = m_wstringToStringCache.get(wstr);
    if (cached.has_value()) {
        stats.stringCacheHits.fetch_add(1);
        return *cached;
    }

    stats.stringCacheMisses.fetch_add(1);

    // Convert
    std::string result = Utils::WStringToString(wstr);

    // Cache the result
    m_wstringToStringCache.put(wstr, result);

    return result;
}

std::wstring ProcessManager::CachedStringToWString(const std::string& str) {
    PERF_TIMER("ProcessManager::StringConversion");

    // Check cache first
    auto cached = m_stringToWstringCache.get(str);
    if (cached.has_value()) {
        stats.stringCacheHits.fetch_add(1);
        return *cached;
    }

    stats.stringCacheMisses.fetch_add(1);

    // Convert
    std::wstring result = Utils::StringToWString(str);

    // Cache the result
    m_stringToWstringCache.put(str, result);

    return result;
}