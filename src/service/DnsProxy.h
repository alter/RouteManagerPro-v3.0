// src/service/DnsProxy.h
#pragma once
#include <winsock2.h>
#include <windows.h>
#include "../../libs/WinDivert/include/windivert.h"
#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <cstdint>

class ProcessManager;
class RouteController;

class DnsProxy {
public:
    DnsProxy(ProcessManager* processManager, RouteController* routeController);
    ~DnsProxy();

    void Start();
    void Stop();
    bool IsActive() const { return active.load(); }

private:
    ProcessManager* processManager;
    RouteController* routeController;

    // WinDivert handles
    HANDLE flowHandle;      // FLOW layer: track DNS flows from selected processes
    HANDLE outboundHandle;  // NETWORK layer: rewrite outbound DNS dst -> 8.8.8.8
    HANDLE inboundHandle;   // NETWORK layer: rewrite inbound DNS src <- original

    std::atomic<bool> running;
    std::atomic<bool> active;

    std::thread flowThread;
    std::thread outboundThread;
    std::thread inboundThread;

    // Target DNS IP (8.8.8.8) in network byte order
    static constexpr uint32_t TARGET_DNS_NBO = 0x08080808;

    static constexpr size_t PACKET_BUFSIZE = 65535;
    static constexpr size_t MAX_FLOW_ENTRIES = 50000;
    static constexpr auto FLOW_EXPIRY = std::chrono::seconds(120);

    // Flow tracking: which (src_ip, src_port) pairs belong to selected processes
    struct FlowKey {
        uint32_t srcIp;
        uint16_t srcPort;

        bool operator==(const FlowKey& other) const {
            return srcIp == other.srcIp && srcPort == other.srcPort;
        }
    };

    struct FlowKeyHash {
        size_t operator()(const FlowKey& k) const {
            return std::hash<uint64_t>()(((uint64_t)k.srcIp << 16) | k.srcPort);
        }
    };

    // Set of active DNS flow keys from selected processes
    struct FlowEntry {
        std::chrono::steady_clock::time_point createdAt;
    };
    std::unordered_map<FlowKey, FlowEntry, FlowKeyHash> trackedFlows;
    mutable std::shared_mutex flowsMutex;

    // NAT table: maps (src_ip, src_port) -> original DNS server IP
    std::unordered_map<FlowKey, uint32_t, FlowKeyHash> natTable;
    std::mutex natMutex;

    // Thread functions
    void FlowThreadFunc();
    void OutboundThreadFunc();
    void InboundThreadFunc();

    // Helpers
    bool IsFlowTracked(const FlowKey& key) const;
    void CleanupExpiredFlows();
    void ParseDnsResponseAndAddRoutes(const uint8_t* dnsPayload, size_t dnsLen);
};
