// src/service/ProcessManager.h
#pragma once
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <optional>
#include <list>
#include "../common/Models.h"
#include "../common/WinHandles.h"

struct CachedProcessInfo {
    bool isSelected;
    FILETIME creationTime;
    std::wstring name;
    std::string processPath;
    std::chrono::steady_clock::time_point lastVerified;
};

struct PerformanceConfig {
    std::chrono::seconds verificationInterval{ 30 };
    size_t missCacheMaxSize{ 1000 };
    size_t mainCacheMaxSize{ 10000 };
    bool aggressiveCaching{ false };
};

struct CacheStats {
    std::atomic<uint64_t> hits{ 0 };
    std::atomic<uint64_t> misses{ 0 };
    std::atomic<uint64_t> verificationChecks{ 0 };
    std::atomic<uint64_t> newProcessChecks{ 0 };
    std::atomic<uint64_t> cacheEvictions{ 0 };
};

template<typename K, typename V>
class ThreadSafeLRUCache {
public:
    ThreadSafeLRUCache(size_t cap) : capacity(cap) {
        if (capacity == 0) capacity = 1;
    }

    void put(const K& key, const V& value) {
        std::unique_lock lock(mutex);

        auto it = map.find(key);
        if (it != map.end()) {
            it->second->value = value;
            list.splice(list.begin(), list, it->second);
            return;
        }

        if (map.size() >= capacity) {
            K last_key = list.back().key;
            list.pop_back();
            map.erase(last_key);
        }

        list.emplace_front(key, value);
        map[key] = list.begin();
    }

    std::optional<V> get(const K& key) const {
        std::shared_lock lock(mutex);

        auto it = map.find(key);
        if (it == map.end()) {
            return std::nullopt;
        }

        return it->second->value;
    }

    void clear() {
        std::unique_lock lock(mutex);
        map.clear();
        list.clear();
    }

    size_t size() const {
        std::shared_lock lock(mutex);
        return map.size();
    }

    template<typename Func>
    void forEach(Func func) const {
        std::shared_lock lock(mutex);
        for (const auto& node : list) {
            func(node.key, node.value);
        }
    }

private:
    struct CacheNode {
        K key;
        V value;
        CacheNode(const K& k, const V& v) : key(k), value(v) {}
    };

    size_t capacity;
    mutable std::list<CacheNode> list;
    mutable std::unordered_map<K, typename std::list<CacheNode>::iterator> map;
    mutable std::shared_mutex mutex;
};

class ProcessManager {
public:
    ProcessManager(const ServiceConfig& config, const PerformanceConfig& perfConfig = {});
    ~ProcessManager();

    std::vector<ProcessInfo> GetAllProcesses() const;
    bool IsProcessSelected(const std::string& processName) const;

    std::optional<CachedProcessInfo> GetCachedInfo(DWORD pid);
    std::optional<CachedProcessInfo> CheckProcessAndCache(DWORD pid);
    void UpdateVerificationTime(DWORD pid);
    void AddToPidCache(DWORD pid, const CachedProcessInfo& info);

    void SetSelectedProcesses(const std::vector<std::string>& processes);
    std::vector<std::string> GetSelectedProcesses() const;

    void GetCacheStats(uint64_t& hits, uint64_t& misses) const;
    void LogPerformanceStats() const;

private:
    mutable std::shared_mutex cachesMutex;
    mutable std::mutex selectedMutex;

    std::unordered_map<DWORD, CachedProcessInfo> m_pidCache;
    ThreadSafeLRUCache<DWORD, CachedProcessInfo> m_pidMissCache;

    std::unordered_set<std::string> selectedProcesses;
    std::vector<ProcessInfo> allProcesses;

    std::atomic<bool> running;
    std::thread updateThread;

    PerformanceConfig perfConfig;
    mutable CacheStats stats;

    void UpdateThreadFunc();
    std::unordered_map<DWORD, CachedProcessInfo> BuildProcessSnapshot();
    std::optional<CachedProcessInfo> GetCompleteProcessInfo(DWORD pid);
    bool MatchesWildcard(const std::string& processName, const std::string& pattern) const;
    bool IsProcessSelectedInternal(const std::string& processName) const;
    void MergeMissCacheIntoMain(std::unordered_map<DWORD, CachedProcessInfo>& mainCache);
    void CleanupStalePids(std::unordered_map<DWORD, CachedProcessInfo>& cache,
        const std::unordered_set<DWORD>& alivePids);
};