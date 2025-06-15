// src/service/RouteOptimizer.cpp
#include "RouteOptimizer.h"
#include "../common/Logger.h"
#include "../service/PerformanceMonitor.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <functional>
#include <cmath>
#include <set>
#include <bit>

RouteOptimizer::RouteOptimizer(const OptimizerConfig& cfg) : config(cfg) {
    Logger::Instance().Info("RouteOptimizer initialized with caching support");
}

void RouteOptimizer::UpdateConfig(const OptimizerConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;

    // Clear cache on config change as results may differ
    std::lock_guard<std::mutex> cacheLock(cacheMutex);
    optimizationCache.clear();
}

OptimizationPlan RouteOptimizer::OptimizeRoutes(const std::vector<HostRoute>& hostRoutes) {
    PERF_TIMER("RouteOptimizer::OptimizeRoutes");
    auto startTime = std::chrono::high_resolution_clock::now();

    // Check cache first
    auto cachedPlan = GetCachedPlan(hostRoutes);
    if (cachedPlan.has_value()) {
        PERF_COUNT("RouteOptimizer.CacheHit");
        Logger::Instance().Debug("RouteOptimizer: Using cached optimization plan");
        return *cachedPlan;
    }

    PERF_COUNT("RouteOptimizer.CacheMiss");
    OptimizationPlan plan;

    // Filter out private IPs
    std::vector<HostRoute> publicRoutes;
    for (const auto& route : hostRoutes) {
        if (!IsPrivateNetwork(route.ipNum)) {
            publicRoutes.push_back(route);
        }
    }

    plan.routesBefore = static_cast<int>(publicRoutes.size());

    if (publicRoutes.size() < config.min_hosts_to_aggregate) {
        Logger::Instance().Info("Not enough public routes to optimize: " +
            std::to_string(publicRoutes.size()));
        plan.routesAfter = plan.routesBefore;
        return plan;
    }

    // Build enhanced trie that handles different prefix lengths
    auto trieRoot = std::make_unique<TrieNode>();
    BuildEnhancedTrie(trieRoot, publicRoutes);

    // Run aggregation with consideration for existing aggregates
    AggregateEnhancedTrie(trieRoot.get(), 0);

    // Generate optimization plan
    std::unordered_map<std::string, RouteInfo> processedRoutes;
    GenerateEnhancedPlan(trieRoot.get(), 0, 0, plan, processedRoutes);

    // Calculate results
    int addedRoutes = 0;
    int removedRoutes = 0;
    for (const auto& change : plan.changes) {
        if (change.type == OptimizationPlan::RouteChange::ADD) {
            addedRoutes++;
        }
        else {
            removedRoutes++;
        }
    }

    plan.routesAfter = plan.routesBefore - removedRoutes + addedRoutes;

    if (plan.routesBefore > 0) {
        plan.compressionRatio = 1.0f - (static_cast<float>(plan.routesAfter) / plan.routesBefore);
    }

    // Update stats
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    {
        std::lock_guard<std::mutex> lock(statsMutex);
        stats.totalOptimizations++;
        stats.totalRoutesProcessed += publicRoutes.size();
        stats.totalRoutesAggregated += removedRoutes;
        stats.totalProcessingTime += duration;
        stats.lastOptimization = std::chrono::system_clock::now();
    }

    // Cache the result
    CachePlan(hostRoutes, plan);

    // Log optimization details
    Logger::Instance().Info("RouteOptimizer: Analyzed " + std::to_string(publicRoutes.size()) +
        " routes, found " + std::to_string(plan.changes.size()) + " changes in " +
        std::to_string(duration.count()) + "ms");

    return plan;
}

RouteOptimizer::Stats RouteOptimizer::GetStats() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return stats;
}

void RouteOptimizer::ResetStats() {
    std::lock_guard<std::mutex> lock(statsMutex);
    stats = Stats();
}

size_t RouteOptimizer::ComputeRouteHash(const std::vector<HostRoute>& routes) const {
    // Create a hash based on sorted routes
    std::vector<HostRoute> sorted = routes;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.ipNum < b.ipNum || (a.ipNum == b.ipNum && a.prefixLength < b.prefixLength);
        });

    std::hash<std::string> hasher;
    size_t hash = 0;

    for (const auto& route : sorted) {
        hash ^= hasher(route.ip) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<int>{}(route.prefixLength) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }

    return hash;
}

void RouteOptimizer::CleanupExpiredCache() {
    auto now = std::chrono::system_clock::now();

    for (auto it = optimizationCache.begin(); it != optimizationCache.end();) {
        if (now - it->second.timestamp > CACHE_EXPIRY) {
            it = optimizationCache.erase(it);
        }
        else {
            ++it;
        }
    }
}

std::optional<OptimizationPlan> RouteOptimizer::GetCachedPlan(const std::vector<HostRoute>& routes) {
    std::lock_guard<std::mutex> lock(cacheMutex);

    CleanupExpiredCache();

    size_t hash = ComputeRouteHash(routes);
    auto it = optimizationCache.find(hash);

    if (it != optimizationCache.end()) {
        // Verify the cached entry matches exactly
        if (it->second.inputRoutes.size() == routes.size()) {
            return it->second.plan;
        }
    }

    return std::nullopt;
}

void RouteOptimizer::CachePlan(const std::vector<HostRoute>& routes, const OptimizationPlan& plan) {
    std::lock_guard<std::mutex> lock(cacheMutex);

    // Limit cache size
    if (optimizationCache.size() >= MAX_CACHE_SIZE) {
        // Remove oldest entry
        auto oldest = optimizationCache.begin();
        for (auto it = optimizationCache.begin(); it != optimizationCache.end(); ++it) {
            if (it->second.timestamp < oldest->second.timestamp) {
                oldest = it;
            }
        }
        optimizationCache.erase(oldest);
    }

    size_t hash = ComputeRouteHash(routes);
    CachedOptimization cached;
    cached.inputRoutes = routes;
    cached.plan = plan;
    cached.timestamp = std::chrono::system_clock::now();

    optimizationCache[hash] = std::move(cached);
}

void RouteOptimizer::BuildEnhancedTrie(std::unique_ptr<TrieNode>& root, const std::vector<HostRoute>& routes) {
    PERF_TIMER("RouteOptimizer::BuildEnhancedTrie");

    for (const auto& route : routes) {
        TrieNode* current = root.get();
        uint32_t ip = route.ipNum;

        // For routes with prefix length < 32, we mark the node at that depth
        int targetDepth = route.prefixLength;

        for (int i = 31; i >= (32 - targetDepth); --i) {
            int bit = (ip >> i) & 1;
            if (!current->children[bit]) {
                current->children[bit] = std::make_unique<TrieNode>();
            }
            current = current->children[bit].get();

            // Mark intermediate nodes if this is an aggregated route
            if (i == (32 - targetDepth)) {
                current->isRoute = true;
                current->prefixLength = route.prefixLength;
                current->processName = route.processName;
                current->routeCount++;

                Logger::Instance().Debug("Added route " + route.ip + "/" +
                    std::to_string(route.prefixLength) + " to trie at depth " +
                    std::to_string(targetDepth));
            }
        }
    }
}

int RouteOptimizer::AggregateEnhancedTrie(TrieNode* node, int depth) {
    if (!node) return 0;

    // If this node represents an existing route, count all routes below it
    if (node->isRoute) {
        return CountRoutesInSubtree(node);
    }

    // Count routes in children
    int leftCount = 0, rightCount = 0;
    if (node->children[0]) {
        leftCount = AggregateEnhancedTrie(node->children[0].get(), depth + 1);
    }
    if (node->children[1]) {
        rightCount = AggregateEnhancedTrie(node->children[1].get(), depth + 1);
    }

    int totalCount = leftCount + rightCount + node->routeCount;

    // Check if we should aggregate at this level
    if (totalCount >= config.min_hosts_to_aggregate && depth < 32) {
        int prefixLen = depth;

        // Check waste threshold
        auto it = config.waste_thresholds.find(prefixLen);
        if (it != config.waste_thresholds.end()) {
            long double totalPossibleHosts = pow(2.0L, 32 - depth);
            float wasteRatio = static_cast<float>((totalPossibleHosts - totalCount) / totalPossibleHosts);

            if (wasteRatio <= it->second) {
                // Only aggregate if it would actually reduce route count
                int existingRouteCount = CountExistingRoutes(node);

                if (existingRouteCount > 1) {  // Only aggregate if we have more than 1 route
                    node->isAggregated = true;
                    PERF_COUNT("RouteOptimizer.Aggregation");
                    Logger::Instance().Debug("Aggregating at depth " + std::to_string(depth) +
                        " with " + std::to_string(totalCount) + " routes (was " +
                        std::to_string(existingRouteCount) + " routes)");
                }
            }
        }
    }

    return totalCount;
}

int RouteOptimizer::CountRoutesInSubtree(TrieNode* node) {
    if (!node) return 0;

    int count = node->routeCount;

    if (node->children[0]) {
        count += CountRoutesInSubtree(node->children[0].get());
    }
    if (node->children[1]) {
        count += CountRoutesInSubtree(node->children[1].get());
    }

    return count;
}

int RouteOptimizer::CountExistingRoutes(TrieNode* node) {
    if (!node) return 0;

    int count = 0;
    if (node->isRoute) count++;

    if (node->children[0]) {
        count += CountExistingRoutes(node->children[0].get());
    }
    if (node->children[1]) {
        count += CountExistingRoutes(node->children[1].get());
    }

    return count;
}

void RouteOptimizer::GenerateEnhancedPlan(TrieNode* node, uint32_t currentSubnet, int depth,
    OptimizationPlan& plan,
    std::unordered_map<std::string, RouteInfo>& processedRoutes) {

    if (!node) return;

    if (node->isAggregated) {
        // Add the new aggregated route
        std::string aggregatedIp = UIntToIP(currentSubnet);
        plan.changes.push_back({
            OptimizationPlan::RouteChange::ADD,
            aggregatedIp,
            depth,
            "Aggregated"
            });

        // Find all existing routes under this node to remove
        std::vector<RouteInfo> routesToRemove;
        CollectRoutesForRemoval(node, currentSubnet, depth, routesToRemove);

        // Add removal entries for each route
        for (const auto& route : routesToRemove) {
            std::string routeKey = route.ip + "/" + std::to_string(route.prefixLength);

            if (processedRoutes.find(routeKey) == processedRoutes.end()) {
                plan.changes.push_back({
                    OptimizationPlan::RouteChange::REMOVE,
                    route.ip,
                    route.prefixLength,
                    route.processName
                    });
                processedRoutes[routeKey] = route;
            }
        }

        return; // Don't process children of aggregated nodes
    }

    // If this is a route that wasn't aggregated, keep it
    if (node->isRoute && !node->isAggregated) {
        // This route stays as-is, no changes needed
    }

    // Continue traversing
    if (depth < 32) {
        if (node->children[0]) {
            GenerateEnhancedPlan(node->children[0].get(), currentSubnet, depth + 1, plan, processedRoutes);
        }
        if (node->children[1]) {
            GenerateEnhancedPlan(node->children[1].get(), currentSubnet | (1u << (31 - depth)), depth + 1, plan, processedRoutes);
        }
    }
}

void RouteOptimizer::CollectRoutesForRemoval(TrieNode* node, uint32_t subnet, int depth,
    std::vector<RouteInfo>& routes) {

    if (!node) return;

    // If this node represents a route, add it to removal list
    if (node->isRoute) {
        RouteInfo info;
        info.ip = UIntToIP(subnet);
        info.prefixLength = node->prefixLength;
        info.processName = node->processName;
        routes.push_back(info);
    }

    // Recursively collect from children
    if (depth < 32) {
        if (node->children[0]) {
            CollectRoutesForRemoval(node->children[0].get(), subnet, depth + 1, routes);
        }
        if (node->children[1]) {
            CollectRoutesForRemoval(node->children[1].get(), subnet | (1u << (31 - depth)), depth + 1, routes);
        }
    }
}

uint32_t RouteOptimizer::CreateMask(int prefixLength) {
    if (prefixLength <= 0) return 0;
    if (prefixLength >= 32) return 0xFFFFFFFF;

    // Optimized version using bit rotation intrinsics
    // Instead of: return ~((1u << (32 - prefixLength)) - 1);
    // We can use std::rotr for potentially better performance on modern CPUs
    return std::rotr(0xFFFFFFFF, 32 - prefixLength);
}

std::string RouteOptimizer::UIntToIP(uint32_t ip) {
    struct in_addr addr;
    addr.s_addr = htonl(ip);
    char buffer[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr, buffer, sizeof(buffer))) {
        return std::string(buffer);
    }
    return "";
}

bool RouteOptimizer::IsPrivateNetwork(uint32_t ip) {
    // Check for private IP ranges using optimized bit operations

    // 10.0.0.0/8 - Check if top 8 bits equal 0x0A
    if ((ip & 0xFF000000) == 0x0A000000) return true;

    // 172.16.0.0/12 - Check if top 12 bits equal 0xAC1
    if ((ip & 0xFFF00000) == 0xAC100000) return true;

    // 192.168.0.0/16 - Check if top 16 bits equal 0xC0A8
    if ((ip & 0xFFFF0000) == 0xC0A80000) return true;

    // 127.0.0.0/8 (loopback) - Check if top 8 bits equal 0x7F
    if ((ip & 0xFF000000) == 0x7F000000) return true;

    return false;
}