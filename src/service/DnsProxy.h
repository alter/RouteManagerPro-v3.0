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
    HANDLE outboundHandle;  // NETWORK layer: rewrite outbound DNS dst -> 8.8.8.8
    HANDLE inboundHandle;   // NETWORK layer: rewrite inbound DNS src <- original

    std::atomic<bool> running;
    std::atomic<bool> active;

    std::thread outboundThread;
    std::thread inboundThread;

    // Target DNS IP (8.8.8.8) in network byte order
    static constexpr uint32_t TARGET_DNS_NBO = 0x08080808;

    static constexpr size_t PACKET_BUFSIZE = 65535;

    // NAT table key: (src_ip, src_port) in network byte order
    struct NatKey {
        uint32_t srcIp;
        uint16_t srcPort;

        bool operator==(const NatKey& other) const {
            return srcIp == other.srcIp && srcPort == other.srcPort;
        }
    };

    struct NatKeyHash {
        size_t operator()(const NatKey& k) const {
            return std::hash<uint64_t>()(((uint64_t)k.srcIp << 16) | k.srcPort);
        }
    };

    // NAT table: maps (src_ip, src_port) -> original DNS server IP
    std::unordered_map<NatKey, uint32_t, NatKeyHash> natTable;
    std::mutex natMutex;

    // Cache of IPs already added as routes (avoid repeated AddRoute calls and ref count inflation)
    std::unordered_set<uint32_t> addedRoutes;
    std::mutex addedRoutesMutex;

    // Thread functions
    void OutboundThreadFunc();
    void InboundThreadFunc();

    // Helpers
    DWORD LookupUdpPid(uint32_t localAddr, uint16_t localPort);
    DWORD LookupTcpPid(uint32_t localAddr, uint16_t localPort);
    void ParseDnsResponseAndAddRoutes(const uint8_t* dnsPayload, size_t dnsLen);
};
