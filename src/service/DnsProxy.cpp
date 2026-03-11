// src/service/DnsProxy.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
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

    // Open NETWORK layer handle for outbound DNS interception (UDP and TCP port 53)
    outboundHandle = WinDivertOpen(
        "outbound and (udp.DstPort == 53 or tcp.DstPort == 53)",
        WINDIVERT_LAYER_NETWORK, 0, 0
    );

    if (outboundHandle == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error(std::format("DnsProxy::Start - Failed to open outbound handle: {}", GetLastError()));
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
        return;
    }

    // Ensure 8.8.8.8 is routable through VPN gateway before intercepting DNS
    if (routeController) {
        routeController->AddRoute("8.8.8.8", "dns-proxy-target");
        Logger::Instance().Info("DnsProxy::Start - Added route for 8.8.8.8 via VPN gateway");
    }

    running = true;
    active = true;

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
    if (outboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertShutdown(outboundHandle, WINDIVERT_SHUTDOWN_BOTH);
    }
    if (inboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertShutdown(inboundHandle, WINDIVERT_SHUTDOWN_BOTH);
    }

    // Join threads
    if (outboundThread.joinable()) {
        outboundThread.join();
    }
    if (inboundThread.joinable()) {
        inboundThread.join();
    }

    // Close handles
    if (outboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertClose(outboundHandle);
        outboundHandle = INVALID_HANDLE_VALUE;
    }
    if (inboundHandle != INVALID_HANDLE_VALUE) {
        WinDivertClose(inboundHandle);
        inboundHandle = INVALID_HANDLE_VALUE;
    }

    // Remove route for 8.8.8.8
    if (routeController) {
        routeController->RemoveRoute("8.8.8.8");
        Logger::Instance().Info("DnsProxy::Stop - Removed route for 8.8.8.8");
    }

    // Clear NAT table
    {
        std::lock_guard lock(natMutex);
        natTable.clear();
    }

    Logger::Instance().Info("DnsProxy::Stop - DNS proxy stopped");
}

DWORD DnsProxy::LookupUdpPid(uint32_t localAddr, uint16_t localPort) {
    // localAddr and localPort are in network byte order (from packet headers)
    // GetExtendedUdpTable also stores them in network byte order

    ULONG size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (size == 0) return 0;

    auto buffer = std::make_unique<uint8_t[]>(size);
    auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buffer.get());

    if (GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) {
        return 0;
    }

    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        const auto& row = table->table[i];
        // row.dwLocalAddr and row.dwLocalPort are in network byte order
        // localAddr == 0.0.0.0 in table means bound to all interfaces
        if ((row.dwLocalAddr == localAddr || row.dwLocalAddr == 0) &&
            static_cast<uint16_t>(row.dwLocalPort) == localPort) {
            return row.dwOwningPid;
        }
    }

    return 0;
}

DWORD DnsProxy::LookupTcpPid(uint32_t localAddr, uint16_t localPort) {
    // localAddr and localPort are in network byte order (from packet headers)

    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return 0;

    auto buffer = std::make_unique<uint8_t[]>(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.get());

    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return 0;
    }

    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        const auto& row = table->table[i];
        if ((row.dwLocalAddr == localAddr || row.dwLocalAddr == 0) &&
            static_cast<uint16_t>(row.dwLocalPort) == localPort) {
            return row.dwOwningPid;
        }
    }

    return 0;
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

        bool isUdp = (udpHdr != nullptr);
        uint16_t srcPort = isUdp ? udpHdr->SrcPort : tcpHdr->SrcPort;
        uint32_t originalDst = ipHdr->DstAddr;

        // Skip if already going to target DNS
        if (originalDst == htonl(TARGET_DNS_NBO)) {
            WinDivertSend(outboundHandle, packet, packetLen, nullptr, &addr);
            continue;
        }

        // Look up PID via system tables
        DWORD pid = isUdp
            ? LookupUdpPid(ipHdr->SrcAddr, srcPort)
            : LookupTcpPid(ipHdr->SrcAddr, srcPort);

        if (pid == 0 || !processManager->IsSelectedProcessByPid(pid)) {
            // Not a selected process — pass through unchanged
            WinDivertSend(outboundHandle, packet, packetLen, nullptr, &addr);
            continue;
        }

        {
            char srcIpStr[INET_ADDRSTRLEN], dstIpStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ipHdr->SrcAddr, srcIpStr, sizeof(srcIpStr));
            inet_ntop(AF_INET, &originalDst, dstIpStr, sizeof(dstIpStr));
            Logger::Instance().Info(std::format("DnsProxy::Outbound - PID {} -> redirecting {}:{} -> {}:53 to 8.8.8.8",
                pid, srcIpStr, ntohs(srcPort), dstIpStr));
        }

        // Store original DNS server in NAT table
        NatKey key{ ipHdr->SrcAddr, srcPort };
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
        NatKey key{ ipHdr->DstAddr, dstPort };

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
