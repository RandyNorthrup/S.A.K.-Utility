// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file tcp_listeners.h
/// @brief The TCP sockets a process is listening on, as Windows reports them.
///
/// For tests of what a server binds: an address the test can see from outside the server, the way
/// netstat -ano shows it, instead of the arguments the server was given.

#pragma once

#include <QHostAddress>
#include <QString>
#include <QStringList>

#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>

namespace sak::test {

namespace detail {

template <class Table, class Row, class Describe>
void appendListeners(ULONG family, DWORD pid, Describe describe, QStringList* out) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> buffer(size);
    if (size == 0 ||
        GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
            NO_ERROR) {
        return;
    }
    const auto* table = reinterpret_cast<const Table*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const Row& row = table->table[index];
        if (row.dwOwningPid == pid) {
            out->append(describe(row));
        }
    }
}

}  // namespace detail

/// "address:port" for every TCP socket @p pid is listening on, IPv4 and IPv6 ("[::]:5201").
[[nodiscard]] inline QStringList tcpListenersOf(DWORD pid) {
    QStringList listeners;
    detail::appendListeners<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(
        AF_INET,
        pid,
        [](const MIB_TCPROW_OWNER_PID& row) {
            return QStringLiteral("%1:%2")
                .arg(QHostAddress(ntohl(row.dwLocalAddr)).toString())
                .arg(ntohs(static_cast<u_short>(row.dwLocalPort)));
        },
        &listeners);
    detail::appendListeners<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>(
        AF_INET6,
        pid,
        [](const MIB_TCP6ROW_OWNER_PID& row) {
            return QStringLiteral("[%1]:%2")
                .arg(QHostAddress(row.ucLocalAddr).toString())
                .arg(ntohs(static_cast<u_short>(row.dwLocalPort)));
        },
        &listeners);
    return listeners;
}

/// The listeners of @p pid on @p port only.
[[nodiscard]] inline QStringList tcpListenersOf(DWORD pid, quint16 port) {
    QStringList on_port;
    const QString suffix = QStringLiteral(":%1").arg(port);
    for (const QString& listener : tcpListenersOf(pid)) {
        if (listener.endsWith(suffix)) {
            on_port.append(listener);
        }
    }
    return on_port;
}

}  // namespace sak::test
