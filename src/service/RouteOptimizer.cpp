// src/service/RouteOptimizer.cpp
#include "RouteOptimizer.h"
#include "../common/Logger.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <functional>
#include <cmath>

RouteOptimizer::RouteOptimizer(const OptimizerConfig& cfg) : config(cfg) {
    Logger::Instance().Info("RouteOptimizer initialized");
}

void RouteOptimizer::UpdateConfig(const OptimizerConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;
}

OptimizationPlan RouteOptimizer::OptimizeRoutes(const std::vector<HostRoute>& hostRoutes) {
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

    // Build trie
    auto trieRoot = std::make_unique<TrieNode>();
    BuildTrie(trieRoot, publicRoutes);

    // Run aggregation
    AggregateTrie(trieRoot.get(), 0);

    // Generate optimization plan
    std::unordered_map<std::string, bool> processedHosts;
    GeneratePlan(trieRoot.get(), 0, 0, plan, processedHosts);

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

    return plan;
}

void RouteOptimizer::BuildTrie(std::unique_ptr<TrieNode>& root, const std::vector<HostRoute>& routes) {
    for (const auto& route : routes) {
        TrieNode* current = root.get();
        uint32_t ip = route.ipNum;

        for (int i = 31; i >= 0; --i) {
            int bit = (ip >> i) & 1;
            if (!current->children[bit]) {
                current->children[bit] = std::make_unique<TrieNode>();
            }
            current = current->children[bit].get();
        }

        current->isHost = true;
        current->processName = route.processName;
    }
}

int RouteOptimizer::AggregateTrie(TrieNode* node, int depth) {
    if (!node) return 0;
    if (node->isHost) return 1;

    int hostCount = AggregateTrie(node->children[0].get(), depth + 1) +
        AggregateTrie(node->children[1].get(), depth + 1);

    if (hostCount >= config.min_hosts_to_aggregate && depth < 32) {
        int prefixLen = depth;

        auto it = config.waste_thresholds.find(prefixLen);
        if (it != config.waste_thresholds.end()) {
            long double totalHostsInSubnet = pow(2.0L, 32 - depth);
            float wasteRatio = static_cast<float>((totalHostsInSubnet - hostCount) / totalHostsInSubnet);

            if (wasteRatio <= it->second) {
                node->isAggregated = true;
                Logger::Instance().Debug("Aggregating at depth " + std::to_string(depth) +
                    " with " + std::to_string(hostCount) + " hosts");
            }
        }
    }

    return hostCount;
}

void RouteOptimizer::GeneratePlan(TrieNode* node, uint32_t currentSubnet, int depth,
    OptimizationPlan& plan,
    std::unordered_map<std::string, bool>& processedHosts) {
    if (!node) return;

    if (node->isAggregated) {
        // Add aggregated route
        plan.changes.push_back({
            OptimizationPlan::RouteChange::ADD,
            UIntToIP(currentSubnet),
            depth,
            "Aggregated"
            });

        // Find all host routes under this aggregated node to remove
        std::function<void(TrieNode*, uint32_t, int)> findHostsToRemove =
            [&](TrieNode* n, uint32_t subnet, int d) {
            if (!n) return;

            if (n->isHost) {
                std::string hostIp = UIntToIP(subnet);
                if (processedHosts.find(hostIp) == processedHosts.end()) {
                    plan.changes.push_back({
                        OptimizationPlan::RouteChange::REMOVE,
                        hostIp,
                        32,
                        n->processName
                        });
                    processedHosts[hostIp] = true;
                }
                return;
            }

            if (d < 32) {
                findHostsToRemove(n->children[0].get(), subnet, d + 1);
                findHostsToRemove(n->children[1].get(), subnet | (1u << (31 - d)), d + 1);
            }
            };

        findHostsToRemove(node, currentSubnet, depth);
        return;
    }

    // If not aggregated, continue traversing
    if (node->isHost) {
        // This is an orphaned host that wasn't aggregated
        return;
    }

    if (depth < 32) {
        GeneratePlan(node->children[0].get(), currentSubnet, depth + 1, plan, processedHosts);
        GeneratePlan(node->children[1].get(), currentSubnet | (1u << (31 - depth)), depth + 1, plan, processedHosts);
    }
}

uint32_t RouteOptimizer::CreateMask(int prefixLength) {
    if (prefixLength <= 0) return 0;
    if (prefixLength >= 32) return 0xFFFFFFFF;
    return ~((1u << (32 - prefixLength)) - 1);
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
    // Check for private IP ranges
    // 10.0.0.0/8
    if ((ip & 0xFF000000) == 0x0A000000) return true;

    // 172.16.0.0/12
    if ((ip & 0xFFF00000) == 0xAC100000) return true;

    // 192.168.0.0/16
    if ((ip & 0xFFFF0000) == 0xC0A80000) return true;

    // 127.0.0.0/8 (loopback)
    if ((ip & 0xFF000000) == 0x7F000000) return true;

    return false;
}