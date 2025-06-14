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
        bool isHost = false;
        bool isAggregated = false;
        std::string processName;
    };

    OptimizerConfig config;
    mutable std::mutex configMutex;

    void BuildTrie(std::unique_ptr<TrieNode>& root, const std::vector<HostRoute>& routes);
    int AggregateTrie(TrieNode* node, int depth);
    void GeneratePlan(TrieNode* node, uint32_t currentSubnet, int depth,
        OptimizationPlan& plan, std::unordered_map<std::string, bool>& processedHosts);

    uint32_t CreateMask(int prefixLength);
    std::string UIntToIP(uint32_t ip);
    bool IsPrivateNetwork(uint32_t ip);
};