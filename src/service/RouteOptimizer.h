// src/service/RouteOptimizer.h
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

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
    int prefixLength = 32;  // Added to support non-/32 routes
};

struct RouteInfo {
    std::string ip;
    int prefixLength;
    std::string processName;
};

class RouteOptimizer {
public:
    RouteOptimizer(const OptimizerConfig& config);
    ~RouteOptimizer() = default;

    // Pure function that takes routes and returns optimization plan
    OptimizationPlan OptimizeRoutes(const std::vector<HostRoute>& hostRoutes);

    void UpdateConfig(const OptimizerConfig& newConfig);

private:
    struct TrieNode {
        std::unique_ptr<TrieNode> children[2];
        bool isRoute = false;          // Marks if this node represents an actual route
        bool isAggregated = false;     // Marks if this subtree should be aggregated
        std::string processName;
        int prefixLength = 32;         // The prefix length of the route at this node
        int routeCount = 0;            // Number of routes at exactly this node
    };

    OptimizerConfig config;
    mutable std::mutex configMutex;

    // Enhanced methods that handle routes with different prefix lengths
    void BuildEnhancedTrie(std::unique_ptr<TrieNode>& root, const std::vector<HostRoute>& routes);
    int AggregateEnhancedTrie(TrieNode* node, int depth);
    void GenerateEnhancedPlan(TrieNode* node, uint32_t currentSubnet, int depth,
        OptimizationPlan& plan, std::unordered_map<std::string, RouteInfo>& processedRoutes);

    // Helper methods
    int CountRoutesInSubtree(TrieNode* node);
    int CountExistingRoutes(TrieNode* node);
    void CollectRoutesForRemoval(TrieNode* node, uint32_t subnet, int depth,
        std::vector<RouteInfo>& routes);

    uint32_t CreateMask(int prefixLength);
    std::string UIntToIP(uint32_t ip);
    bool IsPrivateNetwork(uint32_t ip);
};