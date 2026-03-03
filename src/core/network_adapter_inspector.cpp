// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_adapter_inspector.cpp
/// @brief Adapter enumeration via Windows IP Helper API

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "sak/network_adapter_inspector.h"

#include <QCoreApplication>

#include <memory>

// Link against IP Helper API
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr unsigned long kInitialBufferSize = 15000;
constexpr int kMacAddrLen                  = 6;
constexpr uint64_t kGigabit                = 1'000'000'000ULL;
constexpr uint64_t kMegabit                = 1'000'000ULL;
constexpr uint64_t kKilobit                = 1'000ULL;

constexpr ULONG kGetAdapterFlags = GAA_FLAG_INCLUDE_PREFIX
                               | GAA_FLAG_INCLUDE_GATEWAYS;

bool tryIpv4String(const sockaddr* sa, QString& out)
{
    if (sa == nullptr || sa->sa_family != AF_INET) {
        return false;
    }

    char ipBuf[INET_ADDRSTRLEN] = {};
    auto* sa4 = reinterpret_cast<const sockaddr_in*>(sa);
    inet_ntop(AF_INET, &sa4->sin_addr, ipBuf, sizeof(ipBuf));
    out = QString::fromLatin1(ipBuf);
    return true;
}

bool tryIpv6String(const sockaddr* sa, QString& out)
{
    if (sa == nullptr || sa->sa_family != AF_INET6) {
        return false;
    }

    char ipBuf[INET6_ADDRSTRLEN] = {};
    auto* sa6 = reinterpret_cast<const sockaddr_in6*>(sa);
    inet_ntop(AF_INET6, &sa6->sin6_addr, ipBuf, sizeof(ipBuf));
    out = QString::fromLatin1(ipBuf);
    return true;
}

QString subnetMaskFromPrefixLength(ULONG prefixLength)
{
    if (prefixLength > 32) {
        return {};
    }

    uint32_t mask = 0;
    if (prefixLength > 0) {
        mask = ~((1U << (32U - prefixLength)) - 1U);
    }

    return QString::asprintf("%u.%u.%u.%u",
        (mask >> 24) & 0xFF, (mask >> 16) & 0xFF,
        (mask >> 8) & 0xFF, mask & 0xFF);
}

QString adapterTypeFromIfType(ULONG ifType)
{
    switch (ifType) {
    case IF_TYPE_ETHERNET_CSMACD:
        return QStringLiteral("Ethernet");
    case IF_TYPE_IEEE80211:
        return QStringLiteral("WiFi");
    case IF_TYPE_SOFTWARE_LOOPBACK:
        return QStringLiteral("Loopback");
    case IF_TYPE_TUNNEL:
    case IF_TYPE_PPP:
        return QStringLiteral("VPN");
    default:
        return QStringLiteral("Other");
    }
}

bool tryQueryAdapterAddresses(std::unique_ptr<uint8_t[]>& buffer,
                              PIP_ADAPTER_ADDRESSES& addresses,
                              ULONG& result)
{
    ULONG bufferSize = kInitialBufferSize;
    buffer = std::make_unique<uint8_t[]>(bufferSize);
    addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());

    result = GetAdaptersAddresses(AF_UNSPEC, kGetAdapterFlags, nullptr,
                                 addresses, &bufferSize);
    if (result != ERROR_BUFFER_OVERFLOW) {
        return true;
    }

    buffer = std::make_unique<uint8_t[]>(bufferSize);
    addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());
    result = GetAdaptersAddresses(AF_UNSPEC, kGetAdapterFlags, nullptr,
                                 addresses, &bufferSize);
    return true;
}

void populateDhcpInfo(const IP_ADAPTER_ADDRESSES* addr, sak::NetworkAdapterInfo& info)
{
    info.dhcpEnabled = ((addr->Flags & IP_ADAPTER_DHCP_ENABLED) != 0);
    if (!info.dhcpEnabled || addr->Dhcpv4Server.lpSockaddr == nullptr) {
        return;
    }

    QString ip;
    if (tryIpv4String(addr->Dhcpv4Server.lpSockaddr, ip)) {
        info.dhcpServer = ip;
    }
}

void populateUnicastAddresses(const IP_ADAPTER_ADDRESSES* addr, sak::NetworkAdapterInfo& info)
{
    for (auto* ua = addr->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
        const auto* sa = ua->Address.lpSockaddr;

        QString ip;
        if (tryIpv4String(sa, ip)) {
            info.ipv4Addresses.append(ip);

            const auto maskStr = subnetMaskFromPrefixLength(ua->OnLinkPrefixLength);
            if (!maskStr.isEmpty()) {
                info.ipv4SubnetMasks.append(maskStr);
            }
            continue;
        }

        if (tryIpv6String(sa, ip)) {
            info.ipv6Addresses.append(ip);
        }
    }
}

void populateGateways(const IP_ADAPTER_ADDRESSES* addr, sak::NetworkAdapterInfo& info)
{
    for (auto* gw = addr->FirstGatewayAddress; gw != nullptr; gw = gw->Next) {
        const auto* sa = gw->Address.lpSockaddr;

        QString ip;
        if (tryIpv4String(sa, ip)) {
            info.ipv4Gateway = ip;
            continue;
        }
        if (tryIpv6String(sa, ip)) {
            info.ipv6Gateway = ip;
        }
    }
}

void populateDnsServers(const IP_ADAPTER_ADDRESSES* addr, sak::NetworkAdapterInfo& info)
{
    for (auto* dns = addr->FirstDnsServerAddress; dns != nullptr; dns = dns->Next) {
        const auto* sa = dns->Address.lpSockaddr;

        QString ip;
        if (tryIpv4String(sa, ip)) {
            info.ipv4DnsServers.append(ip);
            continue;
        }
        if (tryIpv6String(sa, ip)) {
            info.ipv6DnsServers.append(ip);
        }
    }
}

void populateIfStats(ULONG ifIndex, sak::NetworkAdapterInfo& info)
{
    MIB_IF_ROW2 ifRow{};
    ifRow.InterfaceIndex = ifIndex;
    if (GetIfEntry2(&ifRow) != NO_ERROR) {
        return;
    }

    info.bytesReceived   = ifRow.InOctets;
    info.bytesSent       = ifRow.OutOctets;
    info.packetsReceived = ifRow.InUcastPkts + ifRow.InNUcastPkts;
    info.packetsSent     = ifRow.OutUcastPkts + ifRow.OutNUcastPkts;
    info.errorsReceived  = ifRow.InErrors;
    info.errorsSent      = ifRow.OutErrors;
}

sak::NetworkAdapterInfo buildAdapterInfo(const IP_ADAPTER_ADDRESSES* addr)
{
    sak::NetworkAdapterInfo info;

    // ── Identity ──
    info.name = QString::fromWCharArray(addr->FriendlyName);
    info.description = QString::fromWCharArray(addr->Description);
    info.interfaceIndex = addr->IfIndex;
    info.adapterType = adapterTypeFromIfType(addr->IfType);

    // MAC
    if (addr->PhysicalAddressLength > 0) {
        info.macAddress = sak::NetworkAdapterInspector::formatMacAddress(
            addr->PhysicalAddress, addr->PhysicalAddressLength);
    }

    // ── Status ──
    info.isConnected = (addr->OperStatus == IfOperStatusUp);
    info.linkSpeedBps = addr->TransmitLinkSpeed;
    info.mediaState = info.isConnected ? QStringLiteral("Connected")
                                      : QStringLiteral("Disconnected");

    // ── DHCP ──
    populateDhcpInfo(addr, info);

    // ── Addresses ──
    populateUnicastAddresses(addr, info);
    populateGateways(addr, info);
    populateDnsServers(addr, info);

    // ── Statistics ──
    populateIfStats(addr->IfIndex, info);

    // ── Driver info ── (from adapter description)
    info.driverName = info.description;

    return info;
}

} // namespace

namespace sak {

NetworkAdapterInspector::NetworkAdapterInspector(QObject* parent)
    : QObject(parent)
{
}

void NetworkAdapterInspector::scan()
{
    auto adapters = enumerateAdapters();
    Q_EMIT scanComplete(adapters);
}

void NetworkAdapterInspector::refresh()
{
    scan();
}

QString NetworkAdapterInspector::formatLinkSpeed(uint64_t bps)
{
    if (bps == 0) {
        return QStringLiteral("N/A");
    }
    if (bps >= kGigabit) {
        const double gbps = static_cast<double>(bps) / static_cast<double>(kGigabit);
        return QString::number(gbps, 'f', (gbps == static_cast<int>(gbps)) ? 0 : 1)
               + QStringLiteral(" Gbps");
    }
    if (bps >= kMegabit) {
        const double mbps = static_cast<double>(bps) / static_cast<double>(kMegabit);
        return QString::number(mbps, 'f', (mbps == static_cast<int>(mbps)) ? 0 : 1)
               + QStringLiteral(" Mbps");
    }
    const double kbps = static_cast<double>(bps) / static_cast<double>(kKilobit);
    return QString::number(kbps, 'f', 0) + QStringLiteral(" Kbps");
}

QString NetworkAdapterInspector::formatMacAddress(const unsigned char* addr,
                                                   unsigned long length)
{
    if (addr == nullptr || length == 0) {
        return {};
    }
    QString mac;
    for (unsigned long i = 0; i < length; ++i) {
        if (i > 0) {
            mac += QLatin1Char(':');
        }
        mac += QString::asprintf("%02X", addr[i]);
    }
    return mac;
}

QVector<NetworkAdapterInfo> NetworkAdapterInspector::enumerateAdapters()
{
    QVector<NetworkAdapterInfo> adapters;

    std::unique_ptr<uint8_t[]> buffer;
    PIP_ADAPTER_ADDRESSES addresses = nullptr;
    ULONG result = NO_ERROR;
    tryQueryAdapterAddresses(buffer, addresses, result);

    if (result != NO_ERROR) {
        Q_EMIT errorOccurred(
            QStringLiteral("GetAdaptersAddresses failed with error %1")
                .arg(result));
        return adapters;
    }

    for (auto addr = addresses; addr != nullptr; addr = addr->Next) {
        adapters.append(buildAdapterInfo(addr));
    }

    return adapters;
}

} // namespace sak
