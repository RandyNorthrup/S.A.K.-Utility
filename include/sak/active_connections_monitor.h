// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file active_connections_monitor.h
/// @brief Real-time TCP/UDP connection monitoring with process mapping

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief Real-time network connection monitor
///
/// Uses GetExtendedTcpTable / GetExtendedUdpTable to enumerate
/// all active TCP connections and UDP listeners with process ID
/// mapping and optional hostname resolution.
class ActiveConnectionsMonitor : public QObject {
    Q_OBJECT

public:
    struct MonitorConfig {
        int refreshIntervalMs      = netdiag::kDefaultConnRefreshMs;
        bool resolveHostnames      = false;
        bool resolveProcessNames   = true;
        bool showTcp               = true;
        bool showUdp               = true;
        QString filterProcessName;
        uint16_t filterPort        = 0;
    };

    explicit ActiveConnectionsMonitor(QObject* parent = nullptr);
    ~ActiveConnectionsMonitor() override;

    ActiveConnectionsMonitor(const ActiveConnectionsMonitor&) = delete;
    ActiveConnectionsMonitor& operator=(const ActiveConnectionsMonitor&) = delete;
    ActiveConnectionsMonitor(ActiveConnectionsMonitor&&) = delete;
    ActiveConnectionsMonitor& operator=(ActiveConnectionsMonitor&&) = delete;

    /// @brief Start periodic monitoring
    void startMonitoring(const MonitorConfig& config = {});

    /// @brief Stop monitoring
    void stopMonitoring();

    /// @brief Perform a single refresh (blocking)
    void refreshNow();

    /// @brief Get the latest connection snapshot
    [[nodiscard]] QVector<ConnectionInfo> getCurrentConnections() const;

Q_SIGNALS:
    void connectionsUpdated(QVector<sak::ConnectionInfo> connections);
    void newConnectionDetected(sak::ConnectionInfo connection);
    void connectionClosed(sak::ConnectionInfo connection);
    void errorOccurred(QString error);

private:
    QTimer* m_refreshTimer = nullptr;  ///< Owned; QObject parent = this
    QVector<ConnectionInfo> m_lastConnections;
    MonitorConfig m_config;
    std::atomic<bool> m_monitoring{false};

    [[nodiscard]] QVector<ConnectionInfo> enumerateTcpConnections();
    [[nodiscard]] QVector<ConnectionInfo> enumerateUdpListeners();
    [[nodiscard]] static QString getProcessName(uint32_t pid);
    [[nodiscard]] static QString getProcessPath(uint32_t pid);
    [[nodiscard]] static QString resolveHostname(const QString& ip);

    void applyFilters(QVector<ConnectionInfo>& connections) const;
    void detectChanges(const QVector<ConnectionInfo>& current);
};

} // namespace sak

static_assert(!std::is_copy_constructible_v<sak::ActiveConnectionsMonitor>,
    "ActiveConnectionsMonitor must not be copyable.");
