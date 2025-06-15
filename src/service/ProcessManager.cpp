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
#include <execution>

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

    updateThread = std::jthread([this](std::stop_token token) { UpdateThreadFunc(token); });
}

ProcessManager::~ProcessManager() {
    Logger::Instance().Debug("ProcessManager::~ProcessManager - Destructor called");
    running = false;
}

bool ProcessManager::IsSelectedProcessByPid(DWORD pid) {
    PERF_TIMER("ProcessManager::IsSelectedProcessByPid");

    // Check cache first
    auto cachedInfo = GetCachedInfo(pid);
    if (cachedInfo.has_value()) {
        stats.hits.fetch_add(1, std::memory_order_relaxed);
        return cachedInfo->isSelected;
    }

    // Cache miss - get complete info
    auto info = GetCompleteProcessInfo(pid);
    if (info.has_value()) {
        // CRITICAL FIX: Add to BOTH caches immediately
        {
            std::unique_lock lock(cachesMutex);

            // Add to main cache if there's room
            if (m_pidCache.size() < perfConfig.mainCacheMaxSize) {
                m_pidCache[pid] = *info;
                Logger::Instance().Debug(std::format("Added PID {} to main cache immediately", pid));
            }
        }

        // Also add to miss cache for redundancy
        m_pidMissCache.put(pid, *info);

        stats.misses.fetch_add(1, std::memory_order_relaxed);
        stats.newProcessChecks.fetch_add(1, std::memory_order_relaxed);
        return info->isSelected;
    }

    stats.misses.fetch_add(1, std::memory_order_relaxed);
    return false;
}

std::optional<CachedProcessInfo> ProcessManager::GetCachedInfo(DWORD pid) {
    // Check main cache first with shared lock
    {
        std::shared_lock lock(cachesMutex);

        if (auto it = m_pidCache.find(pid); it != m_pidCache.end()) {
            // OPTIMIZATION: Update access time for LRU-like behavior
            it->second.lastVerified = std::chrono::steady_clock::now();
            stats.hits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }

    // Check miss cache
    auto missInfo = m_pidMissCache.get(pid);
    if (missInfo.has_value()) {
        // OPTIMIZATION: Promote from miss cache to main cache if frequently accessed
        PromoteToMainCache(pid, *missInfo);
        stats.hits.fetch_add(1, std::memory_order_relaxed);
        return missInfo;
    }

    stats.misses.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

void ProcessManager::PromoteToMainCache(DWORD pid, const CachedProcessInfo& info) {
    std::unique_lock lock(cachesMutex);

    // Only promote if there's room or if we can evict old entries
    if (m_pidCache.size() >= perfConfig.mainCacheMaxSize) {
        // Find and remove the oldest entry
        auto oldest = std::min_element(m_pidCache.begin(), m_pidCache.end(),
            [](const auto& a, const auto& b) {
                return a.second.lastVerified < b.second.lastVerified;
            });

        if (oldest != m_pidCache.end()) {
            Logger::Instance().Debug(std::format("Evicting old PID {} from main cache", oldest->first));
            m_pidCache.erase(oldest);
            stats.cacheEvictions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    m_pidCache[pid] = info;
    Logger::Instance().Debug(std::format("Promoted PID {} from miss cache to main cache", pid));
}

std::optional<CachedProcessInfo> ProcessManager::CheckProcessAndCache(DWORD pid) {
    auto existing = GetCachedInfo(pid);
    if (existing.has_value()) {
        return existing;
    }

    stats.newProcessChecks.fetch_add(1, std::memory_order_relaxed);

    auto info = GetCompleteProcessInfo(pid);
    if (info.has_value() && info->isSelected) {
        // Add to both caches
        {
            std::unique_lock lock(cachesMutex);
            if (m_pidCache.size() < perfConfig.mainCacheMaxSize) {
                m_pidCache[pid] = *info;
            }
        }
        m_pidMissCache.put(pid, *info);
        return info;
    }

    return std::nullopt;
}

void ProcessManager::UpdateVerificationTime(DWORD pid) {
    std::unique_lock lock(cachesMutex);

    if (auto it = m_pidCache.find(pid); it != m_pidCache.end()) {
        it->second.lastVerified = std::chrono::steady_clock::now();
        stats.verificationChecks.fetch_add(1, std::memory_order_relaxed);
    }
}

void ProcessManager::AddToPidCache(DWORD pid, const CachedProcessInfo& info) {
    std::unique_lock lock(cachesMutex);

    if (m_pidCache.size() >= perfConfig.mainCacheMaxSize) {
        // Evict oldest entry
        auto oldest = std::min_element(m_pidCache.begin(), m_pidCache.end(),
            [](const auto& a, const auto& b) {
                return a.second.lastVerified < b.second.lastVerified;
            });

        if (oldest != m_pidCache.end()) {
            m_pidCache.erase(oldest);
            stats.cacheEvictions.fetch_add(1, std::memory_order_relaxed);
        }
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

    if (selectedProcesses.size() > 100) {
        return std::any_of(std::execution::par_unseq,
            selectedProcesses.begin(), selectedProcesses.end(),
            [&processName, this](const auto& selected) {
                if (selected.contains('*')) {
                    return MatchesWildcard(processName, selected);
                }
                return _stricmp(processName.c_str(), selected.c_str()) == 0;
            });
    }
    else {
        return std::ranges::any_of(selectedProcesses, [&processName, this](const auto& selected) {
            if (selected.contains('*')) {
                return MatchesWildcard(processName, selected);
            }
            return _stricmp(processName.c_str(), selected.c_str()) == 0;
            });
    }
}

void ProcessManager::SetSelectedProcesses(const std::vector<std::string>& processes) {
    {
        std::lock_guard<std::mutex> lock(selectedMutex);
        selectedProcesses.clear();
        selectedProcesses.insert(processes.begin(), processes.end());
    }

    // Don't clear caches completely - just mark entries as needing verification
    {
        std::unique_lock lock(cachesMutex);
        for (auto& [pid, info] : m_pidCache) {
            // Recalculate isSelected for existing entries
            info.isSelected = IsProcessSelectedInternal(CachedWStringToString(info.name));
        }
    }

    // Clear miss cache as it might have outdated selection info
    m_pidMissCache.clear();
}

std::vector<std::string> ProcessManager::GetSelectedProcesses() const {
    std::lock_guard<std::mutex> lock(selectedMutex);
    return std::vector<std::string>(selectedProcesses.begin(), selectedProcesses.end());
}

void ProcessManager::UpdateThreadFunc(std::stop_token stopToken) {
    Logger::Instance().Debug("ProcessManager::UpdateThreadFunc - Started");

    std::mutex updateMutex;
    std::condition_variable_any updateCV;

    while (!stopToken.stop_requested() && !ShutdownCoordinator::Instance().isShuttingDown) {
        std::unique_lock<std::mutex> lock(updateMutex);

        if (updateCV.wait_for(lock, stopToken, std::chrono::seconds(5),
            [&stopToken] { return stopToken.stop_requested(); })) {
            break;
        }

        if (stopToken.stop_requested() || ShutdownCoordinator::Instance().isShuttingDown) {
            break;
        }

        try {
            PERF_TIMER("ProcessManager::UpdateSnapshot");

            auto newCache = BuildProcessSnapshot();

            // Merge with existing cache to preserve frequently accessed entries
            MergeWithExistingCache(newCache);

            {
                std::unique_lock lock(cachesMutex);
                m_pidCache = std::move(newCache);

                // Don't clear miss cache - it has valuable data
                // m_pidMissCache.clear();

                allProcesses.clear();
                allProcesses.reserve(m_pidCache.size());

                for (const auto& [pid, info] : m_pidCache) {
                    ProcessInfo procInfo;
                    procInfo.name = info.name;
                    procInfo.executablePath = CachedStringToWString(info.processPath);
                    procInfo.pid = pid;
                    procInfo.isSelected = info.isSelected;
                    procInfo.isGame = Utils::IsGameProcess(CachedWStringToString(info.name));
                    procInfo.isDiscord = Utils::IsDiscordProcess(CachedWStringToString(info.name));
                    allProcesses.push_back(std::move(procInfo));
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

void ProcessManager::MergeWithExistingCache(std::unordered_map<DWORD, CachedProcessInfo>& newCache) {
    std::shared_lock lock(cachesMutex);

    // Preserve access times for existing entries
    for (auto& [pid, newInfo] : newCache) {
        if (auto it = m_pidCache.find(pid); it != m_pidCache.end()) {
            // Preserve the last verified time if the process info hasn't changed
            if (it->second.creationTime.dwLowDateTime == newInfo.creationTime.dwLowDateTime &&
                it->second.creationTime.dwHighDateTime == newInfo.creationTime.dwHighDateTime) {
                newInfo.lastVerified = it->second.lastVerified;
            }
        }
    }
}

std::unordered_map<DWORD, CachedProcessInfo> ProcessManager::BuildProcessSnapshot() {
    PERF_TIMER("ProcessManager::BuildProcessSnapshot");

    std::unordered_map<DWORD, CachedProcessInfo> newCache;
    newCache.reserve(1000);

    std::unordered_set<DWORD> alivePids;
    alivePids.reserve(1000);

    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), HandleDeleter{});
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error("ProcessManager::BuildProcessSnapshot - Failed to create snapshot");
        return newCache;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot.get(), &pe32)) {
        do {
            if (pe32.th32ProcessID == 0 || pe32.th32ProcessID == 4) continue;

            alivePids.insert(pe32.th32ProcessID);

            auto infoOpt = GetCompleteProcessInfo(pe32.th32ProcessID);
            if (infoOpt.has_value()) {
                newCache.emplace(pe32.th32ProcessID, std::move(*infoOpt));
            }

        } while (Process32NextW(snapshot.get(), &pe32));
    }

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
                mainCache.emplace(pid, std::move(*currentInfo));
            }
        }
        });
}

void ProcessManager::GetCacheStats(uint64_t& hits, uint64_t& misses) const {
    hits = stats.hits.load(std::memory_order_relaxed);
    misses = stats.misses.load(std::memory_order_relaxed);
}

void ProcessManager::LogPerformanceStats() const {
    auto hits = stats.hits.load(std::memory_order_relaxed);
    auto misses = stats.misses.load(std::memory_order_relaxed);
    auto verifications = stats.verificationChecks.load(std::memory_order_relaxed);
    auto newChecks = stats.newProcessChecks.load(std::memory_order_relaxed);
    auto stringHits = stats.stringCacheHits.load(std::memory_order_relaxed);
    auto stringMisses = stats.stringCacheMisses.load(std::memory_order_relaxed);

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
    static thread_local std::unordered_map<std::string, bool> matchCache;

    std::string cacheKey = processName + "|" + pattern;
    if (auto it = matchCache.find(cacheKey); it != matchCache.end()) {
        return it->second;
    }

    if (matchCache.size() > 1000) {
        matchCache.clear();
    }

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
            matchCache[cacheKey] = false;
            return false;
        }
    }

    while (patternPos < lowerPattern.length() && lowerPattern[patternPos] == '*') {
        patternPos++;
    }

    bool result = (patternPos == lowerPattern.length());
    matchCache[cacheKey] = result;
    return result;
}

void ProcessManager::CleanupStalePids(std::unordered_map<DWORD, CachedProcessInfo>& cache,
    const std::unordered_set<DWORD>& alivePids) {
    std::erase_if(cache, [&alivePids, this](const auto& pair) {
        bool shouldErase = !alivePids.contains(pair.first);
        if (shouldErase) {
            stats.cacheEvictions.fetch_add(1, std::memory_order_relaxed);
        }
        return shouldErase;
        });
}

std::string ProcessManager::CachedWStringToString(const std::wstring& wstr) {
    PERF_TIMER("ProcessManager::StringConversion");

    auto cached = m_wstringToStringCache.get(wstr);
    if (cached.has_value()) {
        stats.stringCacheHits.fetch_add(1, std::memory_order_relaxed);
        return *cached;
    }

    stats.stringCacheMisses.fetch_add(1, std::memory_order_relaxed);

    std::string result = Utils::WStringToString(wstr);
    m_wstringToStringCache.put(wstr, result);

    return result;
}

std::wstring ProcessManager::CachedStringToWString(const std::string& str) {
    PERF_TIMER("ProcessManager::StringConversion");

    auto cached = m_stringToWstringCache.get(str);
    if (cached.has_value()) {
        stats.stringCacheHits.fetch_add(1, std::memory_order_relaxed);
        return *cached;
    }

    stats.stringCacheMisses.fetch_add(1, std::memory_order_relaxed);

    std::wstring result = Utils::StringToWString(str);
    m_stringToWstringCache.put(str, result);

    return result;
}