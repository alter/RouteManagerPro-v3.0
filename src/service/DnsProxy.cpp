// src/service/DnsProxy.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "DnsProxy.h"
#include "ProcessManager.h"
#include "RouteController.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include "../common/Utils.h"
#include <format>

DnsProxy::DnsProxy(ProcessManager* pm, RouteController* rc)
    : processManager(pm),
    routeController(rc),
    flowHandle(INVALID_HANDLE_VALUE),
    outboundHandle(INVALID_HANDLE_VALUE),
    inboundHandle(INVALID_HANDLE_VALUE),
    running(false),
    active(false) {
    Logger::Instance().Info("DnsProxy: Created");
}

DnsProxy::~DnsProxy() {
    Stop();
}

void DnsProxy::Start() {
    if (running.load()) {
        Logger::Instance().Warning("DnsProxy::Start - Already running");
        return;
    }

    Logger::Instance().Info("DnsProxy::Start - Starting DNS proxy");

    // Open FLOW layer handle to track DNS connections (port 53) from selected processes
    // We use FLOW layer because it provides ProcessId, which NETWORK layer does not
    flowHandle = WinDivertOpen(
        "remotePort == 53 or localPort == 53",
        WINDIVERT_LAYER_FLOW, 1,
        WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY
    );

    if (flowHandle == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error(std::format("DnsProxy::Start - Failed to open FLOW handle: {}", GetLastError()));
        return;
    }

    // Open NETWORK layer handle for outbound DNS interception (UDP and TCP port 53)
    outboundHandle = WinDivertOpen(
        "outbound and (udp.DstPort == 53 or tcp.DstPort == 53)",
        WINDIVERT_LAYER_NETWORK, 0, 0
    );

    if (outboundHandle == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error(std::format("DnsProxy::Start - Failed to open outbound handle: {}", GetLastError()));
        WinDivertClose(flowHandle);
        flowHandle = INVALID_HANDLE_VALUE;
        return;
    }

    // Open NETWORK layer handle for inbound DNS responses from 8.8.8.8
    inboundHandle = WinDivertOpen(
        "inbound and (udp.SrcPort == 53 or tcp.SrcPort == 53) and ip.SrcAddr == 8.8.8.8",
        WINDIVERT_LAYER_NETWORK, 0, 0
    );

    if (inboundHandle == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error(std::format("DnsProxy::Start - Failed to open inbound handle: {}", GetLastError()));
        WinDivertClose(outboundHandle);
        outboundHandle = INVALID_HANDLE_VALUE;
        WinDivertClose(flowHandle);
        flowHandle = INVALID_HANDLE_VALUE;
        return;
    }

    running = true;
    active = true;

    flowThread = std::thread(&DnsProxy::FlowThreadFunc, this);
    outboundThread = std::thread(&DnsProxy::OutboundThreadFunc, this);
    inboundThread = std::thread(&DnsProxy::InboundThreadFunc, this);

    Logger::Instance().Info("DnsProxy::Start - DNS proxy started successfully (UDP + TCP)");
}

void DnsProxy::Stop() {
    if (!running.load()) {
        return;
    }

    Logger::Instance().Info("DnsProxy::Stop - Stopping DNS proxy");

    running = false;
    active = false;

    // Shutdown handles to unblock WinDivertRecv calls
    if (flowHandle != INVALID_HANDLE_VALUE) {
        WinDivertShutdown(flowHandle, WINDIVERT_SHUTDOWN_BOTH);
    }
    if (outboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertShutdown(outboundHandle, WINDIVERT_SHUTDOWN_BOTH);
    }
    if (inboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertShutdown(inboundHandle, WINDIVERT_SHUTDOWN_BOTH);
    }

    // Join threads
    if (flowThread.joinable()) {
        flowThread.join();
    }
    if (outboundThread.joinable()) {
        outboundThread.join();
    }
    if (inboundThread.joinable()) {
        inboundThread.join();
    }

    // Close handles
    if (flowHandle != INVALID_HANDLE_VALUE) {
        WinDivertClose(flowHandle);
        flowHandle = INVALID_HANDLE_VALUE;
    }
    if (outboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertClose(outboundHandle);
        outboundHandle = INVALID_HANDLE_VALUE;
    }
    if (inboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertClose(inboundHandle);
        inboundHandle = INVALID_HANDLE_VALUE;
    }

    // Clear tables
    {
        std::unique_lock lock(flowsMutex);
        trackedFlows.clear();
    }
    {
        std::lock_guard lock(natMutex);
        natTable.clear();
    }

    Logger::Instance().Info("DnsProxy::Stop - DNS proxy stopped");
}

void DnsProxy::FlowThreadFunc() {
    Logger::Instance().Info("DnsProxy::FlowThreadFunc - Flow tracking thread started");

    WINDIVERT_ADDRESS addr;
    uint8_t dummy[1];
    UINT dummyLen = 0;

    auto lastCleanup = std::chrono::steady_clock::now();

    while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
        if (!WinDivertRecv(flowHandle, dummy, sizeof(dummy), &dummyLen, &addr)) {
            if (running.load()) {
                DWORD err = GetLastError();
                if (err != ERROR_NO_DATA && err != ERROR_INVALID_HANDLE) {
                    Logger::Instance().Debug(std::format("DnsProxy::FlowThreadFunc - Recv failed: {}", err));
                }
            }
            break;
        }

        // Only process FLOW_ESTABLISHED events for outbound connections
        if (addr.Event != WINDIVERT_EVENT_FLOW_ESTABLISHED) {
            continue;
        }

        DWORD pid = addr.Flow.ProcessId;
        if (pid == 0) continue;

        // Check if this process is in the selected list
        if (!processManager->IsSelectedProcessByPid(pid)) {
            continue;
        }

        // Extract local IP and port from the flow
        // WinDivert stores IPv4 as IPv4-mapped IPv6 in Flow.LocalAddr
        // For IPv4: LocalAddr[0-2] are 0, LocalAddr[3] contains the IPv4 address
        uint32_t localIp = addr.Flow.LocalAddr[3];
        uint16_t localPort = addr.Flow.LocalPort;

        if (localIp == 0 || localPort == 0) continue;

        FlowKey key{ localIp, htons(localPort) };

        {
            std::unique_lock lock(flowsMutex);
            if (trackedFlows.size() < MAX_FLOW_ENTRIES) {
                trackedFlows[key] = { std::chrono::steady_clock::now() };
            }
        }

        // Periodic cleanup
        auto now = std::chrono::steady_clock::now();
        if (now - lastCleanup > std::chrono::seconds(30)) {
            CleanupExpiredFlows();
            lastCleanup = now;
        }
    }

    Logger::Instance().Info("DnsProxy::FlowThreadFunc - Flow tracking thread exiting");
}

void DnsProxy::OutboundThreadFunc() {
    Logger::Instance().Info("DnsProxy::OutboundThreadFunc - Outbound DNS interception started");

    uint8_t packet[PACKET_BUFSIZE];
    WINDIVERT_ADDRESS addr;
    UINT packetLen;
    PWINDIVERT_IPHDR ipHdr;
    PWINDIVERT_UDPHDR udpHdr;
    PWINDIVERT_TCPHDR tcpHdr;

    while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
        if (!WinDivertRecv(outboundHandle, packet, sizeof(packet), &packetLen, &addr)) {
            if (running.load()) {
                DWORD err = GetLastError();
                if (err != ERROR_NO_DATA && err != ERROR_INVALID_HANDLE) {
                    Logger::Instance().Debug(std::format("DnsProxy::OutboundThreadFunc - Recv failed: {}", err));
                }
            }
            break;
        }

        WinDivertHelperParsePacket(
            packet, packetLen,
            &ipHdr, nullptr, nullptr, nullptr, nullptr,
            &tcpHdr, &udpHdr,
            nullptr, nullptr, nullptr, nullptr
        );

        if (!ipHdr || (!udpHdr && !tcpHdr)) {
            WinDivertSend(outboundHandle, packet, packetLen, nullptr, &addr);
            continue;
        }

        uint16_t srcPort = udpHdr ? udpHdr->SrcPort : (tcpHdr ? tcpHdr->SrcPort : 0);
        uint32_t originalDst = ipHdr->DstAddr;

        // Skip if already going to target DNS
        if (originalDst == htonl(TARGET_DNS_NBO)) {
            WinDivertSend(outboundHandle, packet, packetLen, nullptr, &addr);
            continue;
        }

        // Check if this flow belongs to a selected process
        FlowKey key{ ipHdr->SrcAddr, srcPort };
        if (!IsFlowTracked(key)) {
            // Not a selected process — pass through unchanged
            WinDivertSend(outboundHandle, packet, packetLen, nullptr, &addr);
            continue;
        }

        // Store original DNS server in NAT table
        {
            std::lock_guard lock(natMutex);
            natTable[key] = originalDst;
        }

        // Rewrite destination to 8.8.8.8
        ipHdr->DstAddr = htonl(TARGET_DNS_NBO);
        WinDivertHelperCalcChecksums(packet, packetLen, &addr, 0);

        if (!WinDivertSend(outboundHandle, packet, packetLen, nullptr, &addr)) {
            Logger::Instance().Debug(std::format("DnsProxy::OutboundThreadFunc - Send failed: {}", GetLastError()));
        }
    }

    Logger::Instance().Info("DnsProxy::OutboundThreadFunc - Outbound DNS interception exiting");
}

void DnsProxy::InboundThreadFunc() {
    Logger::Instance().Info("DnsProxy::InboundThreadFunc - Inbound DNS response rewriting started");

    uint8_t packet[PACKET_BUFSIZE];
    WINDIVERT_ADDRESS addr;
    UINT packetLen;
    PWINDIVERT_IPHDR ipHdr;
    PWINDIVERT_UDPHDR udpHdr;
    PWINDIVERT_TCPHDR tcpHdr;

    while (running.load() && !ShutdownCoordinator::Instance().isShuttingDown) {
        if (!WinDivertRecv(inboundHandle, packet, sizeof(packet), &packetLen, &addr)) {
            if (running.load()) {
                DWORD err = GetLastError();
                if (err != ERROR_NO_DATA && err != ERROR_INVALID_HANDLE) {
                    Logger::Instance().Debug(std::format("DnsProxy::InboundThreadFunc - Recv failed: {}", err));
                }
            }
            break;
        }

        WinDivertHelperParsePacket(
            packet, packetLen,
            &ipHdr, nullptr, nullptr, nullptr, nullptr,
            &tcpHdr, &udpHdr,
            nullptr, nullptr, nullptr, nullptr
        );

        if (!ipHdr || (!udpHdr && !tcpHdr)) {
            WinDivertSend(inboundHandle, packet, packetLen, nullptr, &addr);
            continue;
        }

        // For inbound: DstAddr is our local IP, DstPort is our local port
        uint16_t dstPort = udpHdr ? udpHdr->DstPort : (tcpHdr ? tcpHdr->DstPort : 0);
        FlowKey key{ ipHdr->DstAddr, dstPort };

        uint32_t originalDns = 0;
        {
            std::lock_guard lock(natMutex);
            auto it = natTable.find(key);
            if (it != natTable.end()) {
                originalDns = it->second;
                // For UDP, remove after use (single request-response)
                // For TCP, keep entry alive (connection-oriented, multiple exchanges)
                if (udpHdr) {
                    natTable.erase(it);
                }
            }
        }

        if (originalDns != 0) {
            // Parse DNS response and proactively add routes for resolved IPs
            if (routeController && udpHdr) {
                const uint8_t* dnsPayload = reinterpret_cast<const uint8_t*>(udpHdr) + sizeof(WINDIVERT_UDPHDR);
                size_t dnsLen = packetLen - (dnsPayload - packet);
                if (dnsLen >= 12) {
                    ParseDnsResponseAndAddRoutes(dnsPayload, dnsLen);
                }
            }
            else if (routeController && tcpHdr) {
                // TCP DNS: payload starts after TCP header, first 2 bytes are length prefix
                uint32_t tcpHeaderLen = tcpHdr->HdrLength * 4;
                const uint8_t* tcpPayload = reinterpret_cast<const uint8_t*>(tcpHdr) + tcpHeaderLen;
                size_t tcpPayloadLen = packetLen - (tcpPayload - packet);
                if (tcpPayloadLen > 2) {
                    const uint8_t* dnsPayload = tcpPayload + 2; // skip 2-byte length prefix
                    size_t dnsLen = tcpPayloadLen - 2;
                    if (dnsLen >= 12) {
                        ParseDnsResponseAndAddRoutes(dnsPayload, dnsLen);
                    }
                }
            }

            // Rewrite source IP back to original DNS server
            ipHdr->SrcAddr = originalDns;
            WinDivertHelperCalcChecksums(packet, packetLen, &addr, 0);
        }

        if (!WinDivertSend(inboundHandle, packet, packetLen, nullptr, &addr)) {
            Logger::Instance().Debug(std::format("DnsProxy::InboundThreadFunc - Send failed: {}", GetLastError()));
        }
    }

    Logger::Instance().Info("DnsProxy::InboundThreadFunc - Inbound DNS response rewriting exiting");
}

bool DnsProxy::IsFlowTracked(const FlowKey& key) const {
    std::shared_lock lock(flowsMutex);
    return trackedFlows.contains(key);
}

void DnsProxy::CleanupExpiredFlows() {
    auto now = std::chrono::steady_clock::now();
    size_t removed = 0;

    {
        std::unique_lock lock(flowsMutex);
        for (auto it = trackedFlows.begin(); it != trackedFlows.end();) {
            if (now - it->second.createdAt > FLOW_EXPIRY) {
                it = trackedFlows.erase(it);
                removed++;
            }
            else {
                ++it;
            }
        }
    }

    // Also clean up NAT table entries without corresponding flows
    {
        std::lock_guard lock(natMutex);
        for (auto it = natTable.begin(); it != natTable.end();) {
            if (!IsFlowTracked(it->first)) {
                it = natTable.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    if (removed > 0) {
        Logger::Instance().Debug(std::format("DnsProxy: Cleaned up {} expired flow entries", removed));
    }
}

void DnsProxy::ParseDnsResponseAndAddRoutes(const uint8_t* dns, size_t dnsLen) {
    // DNS header: ID(2) Flags(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2)
    if (dnsLen < 12) return;

    uint16_t flags = (dns[2] << 8) | dns[3];
    bool isResponse = (flags & 0x8000) != 0;
    uint8_t rcode = flags & 0x0F;

    if (!isResponse || rcode != 0) return;

    uint16_t qdcount = (dns[4] << 8) | dns[5];
    uint16_t ancount = (dns[6] << 8) | dns[7];

    if (ancount == 0) return;

    // Skip question section
    size_t offset = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        // Skip QNAME
        while (offset < dnsLen) {
            uint8_t labelLen = dns[offset];
            if (labelLen == 0) {
                offset++; // null terminator
                break;
            }
            if ((labelLen & 0xC0) == 0xC0) {
                offset += 2; // compression pointer
                break;
            }
            offset += 1 + labelLen;
        }
        offset += 4; // QTYPE(2) + QCLASS(2)
    }

    // Parse answer section
    for (uint16_t i = 0; i < ancount && offset < dnsLen; i++) {
        // Skip NAME (may be compressed)
        while (offset < dnsLen) {
            uint8_t labelLen = dns[offset];
            if (labelLen == 0) {
                offset++;
                break;
            }
            if ((labelLen & 0xC0) == 0xC0) {
                offset += 2;
                break;
            }
            offset += 1 + labelLen;
        }

        // Need at least TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2) = 10 bytes
        if (offset + 10 > dnsLen) break;

        uint16_t rtype = (dns[offset] << 8) | dns[offset + 1];
        uint16_t rclass = (dns[offset + 2] << 8) | dns[offset + 3];
        // TTL at offset+4..+7 (skip)
        uint16_t rdlength = (dns[offset + 8] << 8) | dns[offset + 9];
        offset += 10;

        if (offset + rdlength > dnsLen) break;

        // A record: type=1, class=IN(1), rdlength=4
        if (rtype == 1 && rclass == 1 && rdlength == 4) {
            std::string ip = std::format("{}.{}.{}.{}",
                dns[offset], dns[offset + 1], dns[offset + 2], dns[offset + 3]);

            if (!Utils::IsPrivateIP(ip)) {
                Logger::Instance().Info(std::format("DnsProxy: DNS resolved -> {}, pre-adding route", ip));
                routeController->AddRoute(ip, "dns-prefetch");
            }
        }

        offset += rdlength;
    }
}
