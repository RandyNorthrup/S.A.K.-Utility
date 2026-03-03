// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file active_connections_monitor.cpp
/// @brief Real-time TCP/UDP connection monitoring with process mapping

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "sak/active_connections_monitor.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace sak {

namespace {
constexpr DWORD kInitialBufferSize = 32768;
constexpr int kMaxHostnameLen      = 256;

[[nodiscard]] QString ipv4ToString(DWORD addr)
{
    IN_ADDR in;
    in.S_un.S_addr = addr;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &in, buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

[[nodiscard]] QString tcpStateToString(DWORD state)
{
    switch (state) {
    case MIB_TCP_STATE_CLOSED:       return QStringLiteral("CLOSED");
    case MIB_TCP_STATE_LISTEN:       return QStringLiteral("LISTEN");
    case MIB_TCP_STATE_SYN_SENT:     return QStringLiteral("SYN_SENT");
    case MIB_TCP_STATE_SYN_RCVD:     return QStringLiteral("SYN_RCVD");
    case MIB_TCP_STATE_ESTAB:        return QStringLiteral("ESTABLISHED");
    case MIB_TCP_STATE_FIN_WAIT1:    return QStringLiteral("FIN_WAIT1");
    case MIB_TCP_STATE_FIN_WAIT2:    return QStringLiteral("FIN_WAIT2");
    case MIB_TCP_STATE_CLOSE_WAIT:   return QStringLiteral("CLOSE_WAIT");
    case MIB_TCP_STATE_CLOSING:      return QStringLiteral("CLOSING");
    case MIB_TCP_STATE_LAST_ACK:     return QStringLiteral("LAST_ACK");
    case MIB_TCP_STATE_TIME_WAIT:    return QStringLiteral("TIME_WAIT");
    case MIB_TCP_STATE_DELETE_TCB:   return QStringLiteral("DELETE_TCB");
    default:                         return QStringLiteral("UNKNOWN");
    }
}
} // namespace

ActiveConnectionsMonitor::ActiveConnectionsMonitor(QObject* parent)
    : QObject(parent)
{
}

ActiveConnectionsMonitor::~ActiveConnectionsMonitor()
{
    stopMonitoring();
}

void ActiveConnectionsMonitor::startMonitoring(const MonitorConfig& config)
{
    stopMonitoring();
    m_config = config;
    m_monitoring.store(true);

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &ActiveConnectionsMonitor::refreshNow);
    m_refreshTimer->start(config.refreshIntervalMs);

    // Initial refresh
    refreshNow();
}

void ActiveConnectionsMonitor::stopMonitoring()
{
    m_monitoring.store(false);
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
        delete m_refreshTimer;
        m_refreshTimer = nullptr;
    }
}

void ActiveConnectionsMonitor::refreshNow()
{
    QVector<ConnectionInfo> connections;

    if (m_config.showTcp) {
        connections.append(enumerateTcpConnections());
    }
    if (m_config.showUdp) {
        connections.append(enumerateUdpListeners());
    }

    applyFilters(connections);
    detectChanges(connections);

    m_lastConnections = connections;
    Q_EMIT connectionsUpdated(connections);
}

QVector<ConnectionInfo> ActiveConnectionsMonitor::getCurrentConnections() const
{
    return m_lastConnections;
}

QVector<ConnectionInfo> ActiveConnectionsMonitor::enumerateTcpConnections()
{
    QVector<ConnectionInfo> connections;

    DWORD bufferSize = kInitialBufferSize;
    auto buffer = std::make_unique<BYTE[]>(bufferSize);

    DWORD result = GetExtendedTcpTable(
        buffer.get(), &bufferSize, TRUE, AF_INET,
        TCP_TABLE_OWNER_PID_ALL, 0);

    if (result == ERROR_INSUFFICIENT_BUFFER) {
        buffer = std::make_unique<BYTE[]>(bufferSize);
        result = GetExtendedTcpTable(
            buffer.get(), &bufferSize, TRUE, AF_INET,
            TCP_TABLE_OWNER_PID_ALL, 0);
    }

    if (result != NO_ERROR) {
        return connections;
    }

    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.get());

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];

        ConnectionInfo info;
        info.protocol = ConnectionInfo::Protocol::TCP;
        info.localAddress = ipv4ToString(row.dwLocalAddr);
        info.localPort = static_cast<uint16_t>(ntohs(static_cast<u_short>(row.dwLocalPort)));
        info.remoteAddress = ipv4ToString(row.dwRemoteAddr);
        info.remotePort = static_cast<uint16_t>(ntohs(static_cast<u_short>(row.dwRemotePort)));
        info.state = tcpStateToString(row.dwState);
        info.processId = row.dwOwningPid;

        if (m_config.resolveProcessNames) {
            info.processName = getProcessName(row.dwOwningPid);
            info.processPath = getProcessPath(row.dwOwningPid);
        }

        if (m_config.resolveHostnames &&
            info.remoteAddress != QStringLiteral("0.0.0.0") &&
            info.remoteAddress != QStringLiteral("127.0.0.1")) {
            info.remoteHostname = resolveHostname(info.remoteAddress);
        }

        connections.append(info);
    }

    return connections;
}

QVector<ConnectionInfo> ActiveConnectionsMonitor::enumerateUdpListeners()
{
    QVector<ConnectionInfo> connections;

    DWORD bufferSize = kInitialBufferSize;
    auto buffer = std::make_unique<BYTE[]>(bufferSize);

    DWORD result = GetExtendedUdpTable(
        buffer.get(), &bufferSize, TRUE, AF_INET,
        UDP_TABLE_OWNER_PID, 0);

    if (result == ERROR_INSUFFICIENT_BUFFER) {
        buffer = std::make_unique<BYTE[]>(bufferSize);
        result = GetExtendedUdpTable(
            buffer.get(), &bufferSize, TRUE, AF_INET,
            UDP_TABLE_OWNER_PID, 0);
    }

    if (result != NO_ERROR) {
        return connections;
    }

    auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buffer.get());

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];

        ConnectionInfo info;
        info.protocol = ConnectionInfo::Protocol::UDP;
        info.localAddress = ipv4ToString(row.dwLocalAddr);
        info.localPort = static_cast<uint16_t>(ntohs(static_cast<u_short>(row.dwLocalPort)));
        info.state = QStringLiteral("LISTENING");
        info.processId = row.dwOwningPid;

        if (m_config.resolveProcessNames) {
            info.processName = getProcessName(row.dwOwningPid);
            info.processPath = getProcessPath(row.dwOwningPid);
        }

        connections.append(info);
    }

    return connections;
}

QString ActiveConnectionsMonitor::getProcessName(uint32_t pid)
{
    if (pid == 0) {
        return QStringLiteral("System Idle");
    }
    if (pid == 4) {
        return QStringLiteral("System");
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return QStringLiteral("[PID %1]").arg(pid);
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    QString name = QStringLiteral("[PID %1]").arg(pid);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                name = QString::fromWCharArray(entry.szExeFile);
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return name;
}

QString ActiveConnectionsMonitor::getProcessPath(uint32_t pid)
{
    if (pid == 0 || pid == 4) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }

    wchar_t path[MAX_PATH] = {};
    DWORD pathLen = MAX_PATH;
    QString result;

    if (QueryFullProcessImageNameW(process, 0, path, &pathLen)) {
        result = QString::fromWCharArray(path, static_cast<int>(pathLen));
    }

    CloseHandle(process);
    return result;
}

QString ActiveConnectionsMonitor::resolveHostname(const QString& ip)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip.toLatin1().constData(), &addr.sin_addr);

    char host[kMaxHostnameLen] = {};
    const int result = getnameinfo(
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr),
        host, sizeof(host),
        nullptr, 0,
        NI_NAMEREQD);

    if (result == 0) {
        return QString::fromLatin1(host);
    }
    return {};
}

void ActiveConnectionsMonitor::applyFilters(QVector<ConnectionInfo>& connections) const
{
    if (m_config.filterProcessName.isEmpty() && m_config.filterPort == 0) {
        return;
    }

    connections.erase(
        std::remove_if(connections.begin(), connections.end(),
                        [this](const ConnectionInfo& c) {
                            if (!m_config.filterProcessName.isEmpty() &&
                                !c.processName.contains(m_config.filterProcessName,
                                                         Qt::CaseInsensitive)) {
                                return true;
                            }
                            if (m_config.filterPort != 0 &&
                                c.localPort != m_config.filterPort &&
                                c.remotePort != m_config.filterPort) {
                                return true;
                            }
                            return false;
                        }),
        connections.end());
}

void ActiveConnectionsMonitor::detectChanges(
    const QVector<ConnectionInfo>& current)
{
    if (m_lastConnections.isEmpty()) {
        return;
    }

    // Helper to create a unique key for a connection
    auto key = [](const ConnectionInfo& c) -> QString {
        return QStringLiteral("%1:%2-%3:%4-%5-%6")
            .arg(c.localAddress)
            .arg(c.localPort)
            .arg(c.remoteAddress)
            .arg(c.remotePort)
            .arg(static_cast<int>(c.protocol))
            .arg(c.processId);
    };

    QSet<QString> oldKeys;
    QHash<QString, ConnectionInfo> oldMap;
    for (const auto& c : m_lastConnections) {
        const auto k = key(c);
        oldKeys.insert(k);
        oldMap.insert(k, c);
    }

    QSet<QString> newKeys;
    for (const auto& c : current) {
        const auto k = key(c);
        newKeys.insert(k);
        if (!oldKeys.contains(k)) {
            Q_EMIT newConnectionDetected(c);
        }
    }

    for (const auto& k : oldKeys) {
        if (!newKeys.contains(k)) {
            Q_EMIT connectionClosed(oldMap.value(k));
        }
    }
}

} // namespace sak
