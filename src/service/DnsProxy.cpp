// src/service/DnsProxy.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "DnsProxy.h"
#include "ProcessManager.h"
#include "../common/Logger.h"
#include "../common/ShutdownCoordinator.h"
#include <format>

DnsProxy::DnsProxy(ProcessManager* pm)
    : processManager(pm),
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
        "tcp.DstPort == 53 or udp.DstPort == 53 or tcp.SrcPort == 53 or udp.SrcPort == 53",
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
