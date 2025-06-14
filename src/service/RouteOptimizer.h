// src/service/RouteOptimizer.h
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>
#include "../common/Models.h"

struct OptimizerConfig {
    int min_hosts_to_aggregate = 2;
    std::unordered_map<int, float> waste_thresholds = {
        {30, 0.75f}, {29, 0.80f}, {28, 0.85f},
        {27, 0.90f}, {26, 0.90f}, {25, 0.92f}, {24, 0.95f}
    };
};

struct OptimizationPlan {
    struct RouteChange {
        enum Type { ADD, REMOVE };
        Type type;
        std::string ip;
        int prefixLength;
        std::string reason;
    };

    std::vector<RouteChange> changes;
    int routesBefore = 0;
    int routesAfter = 0;
    float compressionRatio = 0.0f;
};

struct HostRoute {
    std::string ip;
    uint32_t ipNum;
    std::string processName;
    int prefixLength = 32;
};

class RouteOptimizer {
public:
    RouteOptimizer(const OptimizerConfig& config);
    ~RouteOptimizer() = default;

    OptimizationPlan OptimizeRoutes(const std::vector<HostRoute>& hostRoutes);
    void UpdateConfig(const OptimizerConfig& newConfig);

    // Performance stats
    struct Stats {
        uint64_t totalOptimizations = 0;
        uint64_t totalRoutesProcessed = 0;
        uint64_t totalRoutesAggregated = 0;
        std::chrono::milliseconds totalProcessingTime{ 0 };
        std::chrono::system_clock::time_point lastOptimization;
    };

    Stats GetStats() const;
    void ResetStats();

private:
    struct TrieNode {
        std::unique_ptr<TrieNode> children[2];
        bool isRoute = false;
        bool isAggregated = false;
        std::string processName;
        int prefixLength = 32;
        int routeCount = 0;
    };

    struct CachedOptimization {
        std::vector<HostRoute> inputRoutes;
        OptimizationPlan plan;
        std::chrono::system_clock::time_point timestamp;
    };

    OptimizerConfig config;
    mutable std::mutex configMutex;

    // Performance tracking
    mutable Stats stats;
    mutable std::mutex statsMutex;

    // Cache for recent optimizations
    std::unordered_map<size_t, CachedOptimization> optimizationCache;
    mutable std::mutex cacheMutex;
    static constexpr size_t MAX_CACHE_SIZE = 10;
    static constexpr auto CACHE_EXPIRY = std::chrono::minutes(5);

    void BuildEnhancedTrie(std::unique_ptr<TrieNode>& root, const std::vector<HostRoute>& routes);
    int AggregateEnhancedTrie(TrieNode* node, int depth);
    void GenerateEnhancedPlan(TrieNode* node, uint32_t currentSubnet, int depth,
        OptimizationPlan& plan, std::unordered_map<std::string, RouteInfo>& processedRoutes);

    int CountRoutesInSubtree(TrieNode* node);
    int CountExistingRoutes(TrieNode* node);
    void CollectRoutesForRemoval(TrieNode* node, uint32_t subnet, int depth,
        std::vector<RouteInfo>& routes);

    uint32_t CreateMask(int prefixLength);
    std::string UIntToIP(uint32_t ip);
    bool IsPrivateNetwork(uint32_t ip);

    // Cache helpers
    size_t ComputeRouteHash(const std::vector<HostRoute>& routes) const;
    void CleanupExpiredCache();
    std::optional<OptimizationPlan> GetCachedPlan(const std::vector<HostRoute>& routes);
    void CachePlan(const std::vector<HostRoute>& routes, const OptimizationPlan& plan);
};