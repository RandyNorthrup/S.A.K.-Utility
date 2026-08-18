// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file active_connections_monitor.cpp
/// @brief Real-time TCP/UDP connection monitoring with process mapping

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "sak/active_connections_monitor.h"

#include <QtGlobal>

#include <algorithm>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace sak {

namespace {
constexpr uint32_t kWindowsSystemProcessId = 4;
// Positive floor for the refresh timer. A 0 ms (or negative) interval would fire on every
// event-loop turn and re-enumerate the TCP/UDP tables continuously (CPU spin); clamp up to
// this floor. The default and every legitimate configured interval sit well above it.
constexpr int kMinConnRefreshMs = 100;
}  // namespace

namespace {
constexpr DWORD kInitialBufferSize = 32'768;
constexpr int kMaxHostnameLen = 256;

[[nodiscard]] QString ipv4ToString(DWORD addr) {
    IN_ADDR in;
    in.S_un.S_addr = addr;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &in, buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

[[nodiscard]] QString tcpStateToString(DWORD state) {
    struct StateEntry {
        DWORD code;
        const char* name;
    };
    static constexpr StateEntry kStates[] = {
        {.code = MIB_TCP_STATE_CLOSED, .name = "CLOSED"},
        {.code = MIB_TCP_STATE_LISTEN, .name = "LISTEN"},
        {.code = MIB_TCP_STATE_SYN_SENT, .name = "SYN_SENT"},
        {.code = MIB_TCP_STATE_SYN_RCVD, .name = "SYN_RCVD"},
        {.code = MIB_TCP_STATE_ESTAB, .name = "ESTABLISHED"},
        {.code = MIB_TCP_STATE_FIN_WAIT1, .name = "FIN_WAIT1"},
        {.code = MIB_TCP_STATE_FIN_WAIT2, .name = "FIN_WAIT2"},
        {.code = MIB_TCP_STATE_CLOSE_WAIT, .name = "CLOSE_WAIT"},
        {.code = MIB_TCP_STATE_CLOSING, .name = "CLOSING"},
        {.code = MIB_TCP_STATE_LAST_ACK, .name = "LAST_ACK"},
        {.code = MIB_TCP_STATE_TIME_WAIT, .name = "TIME_WAIT"},
        {.code = MIB_TCP_STATE_DELETE_TCB, .name = "DELETE_TCB"},
    };
    for (const auto& entry : kStates) {
        if (entry.code == state) {
            return QString::fromLatin1(entry.name);
        }
    }
    return QStringLiteral("UNKNOWN");
}
}  // namespace

ActiveConnectionsMonitor::ActiveConnectionsMonitor(QObject* parent) : QObject(parent) {}

ActiveConnectionsMonitor::~ActiveConnectionsMonitor() {
    stopMonitoring();
}

void ActiveConnectionsMonitor::startMonitoring(const MonitorConfig& config) {
    stopMonitoring();
    m_config = config;

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &ActiveConnectionsMonitor::refreshNow);
    // Clamp to a positive floor so a 0 ms (or negative) configured interval cannot spin the
    // event loop; a real interval is never below the floor, so no legitimate config is altered.
    m_refreshTimer->start(std::max(config.refreshIntervalMs, kMinConnRefreshMs));

    // Initial refresh
    refreshNow();
}

void ActiveConnectionsMonitor::stopMonitoring() {
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
        delete m_refreshTimer;
        m_refreshTimer = nullptr;
    }
}

void ActiveConnectionsMonitor::refreshNow() {
    QVector<ConnectionInfo> connections;

    // Resolve every PID to a name from a SINGLE process snapshot for this whole refresh, rather
    // than snapshotting once per connection (was O(connections * processes) -- a real cost on a
    // high-socket host and on every periodic GUI refresh). Paths are still resolved per-PID.
    const QHash<uint32_t, QString> processNames =
        m_config.resolveProcessNames ? snapshotProcessNames() : QHash<uint32_t, QString>{};

    quint32 tcpError = 0;
    quint32 udpError = 0;
    if (m_config.showTcp) {
        connections.append(enumerateTcpConnections(processNames, tcpError));
    }
    if (m_config.showUdp) {
        connections.append(enumerateUdpListeners(processNames, udpError));
    }
    m_lastRefreshError = (tcpError != 0) || (udpError != 0);
    if (m_lastRefreshError) {
        // A TCP/UDP table read failed, so `connections` is empty or missing a
        // whole protocol. Diffing against it would report every baseline
        // connection as closed and then wipe the baseline, and the next
        // successful refresh would re-report them all as new. Preserve the last
        // good snapshot and skip the diff on a read failure (B9-08).
        //
        // Fail closed: surface the exact kernel error instead of quietly
        // re-publishing stale data as if the refresh succeeded.
        QString detail;
        if (tcpError != 0) {
            detail = QStringLiteral("TCP connection table read failed (error %1)").arg(tcpError);
        }
        if (udpError != 0) {
            if (!detail.isEmpty()) {
                detail += QStringLiteral("; ");
            }
            detail += QStringLiteral("UDP listener table read failed (error %1)").arg(udpError);
        }
        Q_EMIT errorOccurred(detail);
        Q_EMIT connectionsUpdated(m_lastConnections);
        return;
    }

    applyFilters(connections);
    detectChanges(connections);

    m_lastConnections = connections;
    m_hasBaseline = true;
    Q_EMIT connectionsUpdated(connections);
}

QVector<ConnectionInfo> ActiveConnectionsMonitor::getCurrentConnections() const {
    return m_lastConnections;
}

QVector<ConnectionInfo> ActiveConnectionsMonitor::enumerateTcpConnections(
    const QHash<uint32_t, QString>& processNames, quint32& readError) {
    QVector<ConnectionInfo> connections;

    DWORD bufferSize = kInitialBufferSize;
    auto buffer = std::make_unique<BYTE[]>(bufferSize);

    DWORD result =
        GetExtendedTcpTable(buffer.get(), &bufferSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);

    if (result == ERROR_INSUFFICIENT_BUFFER) {
        buffer = std::make_unique<BYTE[]>(bufferSize);
        result = GetExtendedTcpTable(
            buffer.get(), &bufferSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    }

    if (result != NO_ERROR) {
        readError = static_cast<quint32>(result);
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
            info.processName = lookupProcessName(row.dwOwningPid, processNames);
            info.processPath = getProcessPath(row.dwOwningPid);
        }

        if (m_config.resolveHostnames && info.remoteAddress != QStringLiteral("0.0.0.0") &&
            info.remoteAddress != QStringLiteral("127.0.0.1")) {
            info.remoteHostname = resolveHostname(info.remoteAddress);
        }

        connections.append(info);
    }

    return connections;
}

QVector<ConnectionInfo> ActiveConnectionsMonitor::enumerateUdpListeners(
    const QHash<uint32_t, QString>& processNames, quint32& readError) {
    QVector<ConnectionInfo> connections;

    DWORD bufferSize = kInitialBufferSize;
    auto buffer = std::make_unique<BYTE[]>(bufferSize);

    DWORD result =
        GetExtendedUdpTable(buffer.get(), &bufferSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);

    if (result == ERROR_INSUFFICIENT_BUFFER) {
        buffer = std::make_unique<BYTE[]>(bufferSize);
        result =
            GetExtendedUdpTable(buffer.get(), &bufferSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    }

    if (result != NO_ERROR) {
        readError = static_cast<quint32>(result);
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
            info.processName = lookupProcessName(row.dwOwningPid, processNames);
            info.processPath = getProcessPath(row.dwOwningPid);
        }

        connections.append(info);
    }

    return connections;
}

QHash<uint32_t, QString> ActiveConnectionsMonitor::snapshotProcessNames() {
    QHash<uint32_t, QString> names;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return names;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry) != 0) {
        do {
            names.insert(entry.th32ProcessID, QString::fromWCharArray(entry.szExeFile));
        } while (Process32NextW(snapshot, &entry) != 0);
    }

    CloseHandle(snapshot);
    return names;
}

QString ActiveConnectionsMonitor::lookupProcessName(uint32_t pid,
                                                    const QHash<uint32_t, QString>& processNames) {
    if (pid == 0) {
        return QStringLiteral("System Idle");
    }
    if (pid == kWindowsSystemProcessId) {
        return QStringLiteral("System");
    }
    return processNames.value(pid, QStringLiteral("[PID %1]").arg(pid));
}

QString ActiveConnectionsMonitor::getProcessPath(uint32_t pid) {
    if (pid == 0 || pid == kWindowsSystemProcessId) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }

    // A process image path can exceed MAX_PATH on a long-path-enabled system;
    // QueryFullProcessImageNameW then fails with ERROR_INSUFFICIENT_BUFFER rather than
    // truncating, which would drop the name to a bare [PID] in the monitor. Oversize the
    // buffer to twice MAX_PATH, matching the module-path handling in browser_bridge_pipe, so a
    // long path resolves. A genuine failure (access denied, process exited) still leaves the
    // name empty, which the caller renders fail-closed as [PID N].
    constexpr DWORD kProcessPathBufferChars = MAX_PATH * 2;
    wchar_t path[kProcessPathBufferChars] = {};
    DWORD pathLen = kProcessPathBufferChars;
    QString result;

    if (QueryFullProcessImageNameW(process, 0, path, &pathLen) != 0) {
        result = QString::fromWCharArray(path, static_cast<int>(pathLen));
    }

    CloseHandle(process);
    return result;
}

QString ActiveConnectionsMonitor::resolveHostname(const QString& ip) {
    // Sole caller passes ipv4ToString(), whose inet_ntop into an INET_ADDRSTRLEN buffer always
    // yields a dotted quad for AF_INET.
    Q_ASSERT(!ip.isEmpty());
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip.toLatin1().constData(), &addr.sin_addr);

    char host[kMaxHostnameLen] = {};
    const int result = getnameinfo(reinterpret_cast<sockaddr*>(&addr),
                                   sizeof(addr),
                                   host,
                                   sizeof(host),
                                   nullptr,
                                   0,
                                   NI_NAMEREQD);

    if (result == 0) {
        return QString::fromLatin1(host);
    }
    return {};
}

void ActiveConnectionsMonitor::applyFilters(QVector<ConnectionInfo>& connections) const {
    if (m_config.filterProcessName.isEmpty() && m_config.filterPort == 0) {
        return;
    }

    const auto stale = std::ranges::remove_if(connections, [this](const ConnectionInfo& c) {
        if (!m_config.filterProcessName.isEmpty() &&
            !c.processName.contains(m_config.filterProcessName, Qt::CaseInsensitive)) {
            return true;
        }
        if (m_config.filterPort != 0 && c.localPort != m_config.filterPort &&
            c.remotePort != m_config.filterPort) {
            return true;
        }
        return false;
    });
    connections.erase(stale.begin(), stale.end());
}

void ActiveConnectionsMonitor::detectChanges(const QVector<ConnectionInfo>& current) {
    // Skip only until a first baseline exists -- NOT whenever the previous snapshot
    // was empty. Using isEmpty() as the sentinel meant that if the prior refresh
    // legitimately had zero connections, the next refresh's genuinely new connections
    // were suppressed. m_hasBaseline distinguishes "no baseline yet" from "empty baseline".
    if (!m_hasBaseline) {
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

}  // namespace sak
