// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file active_connections_monitor.h
/// @brief Real-time TCP/UDP connection monitoring with process mapping

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <cstdint>
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
        int refreshIntervalMs = netdiag::kDefaultConnRefreshMs;
        bool resolveHostnames = false;
        bool resolveProcessNames = true;
        bool showTcp = true;
        bool showUdp = true;
        QString filterProcessName;
        uint16_t filterPort = 0;
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

    /// @brief True if the most recent refresh hit a kernel table-read error (a requested
    /// TCP/UDP table returned other than NO_ERROR). Distinguishes a genuine "no connections"
    /// (NO_ERROR, zero entries) from a failed read (e.g. ERROR_NOT_SUPPORTED) so a caller can
    /// report the failure honestly instead of a misleading empty success. Additive; the GUI
    /// path ignores it and is unaffected.
    [[nodiscard]] bool lastRefreshHadError() const noexcept { return m_lastRefreshError; }

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
    bool m_lastRefreshError = false;
    bool m_hasBaseline = false;  ///< True once a first successful refresh established a baseline

    [[nodiscard]] QVector<ConnectionInfo> enumerateTcpConnections(
        const QHash<uint32_t, QString>& processNames, bool& readError);
    [[nodiscard]] QVector<ConnectionInfo> enumerateUdpListeners(
        const QHash<uint32_t, QString>& processNames, bool& readError);
    /// Build a PID -> process-name map from ONE Toolhelp snapshot, so a refresh pays a single
    /// snapshot instead of one per connection (was O(connections * processes)).
    [[nodiscard]] static QHash<uint32_t, QString> snapshotProcessNames();
    [[nodiscard]] static QString lookupProcessName(uint32_t pid,
                                                   const QHash<uint32_t, QString>& processNames);
    [[nodiscard]] static QString getProcessPath(uint32_t pid);
    [[nodiscard]] static QString resolveHostname(const QString& ip);

    void applyFilters(QVector<ConnectionInfo>& connections) const;
    void detectChanges(const QVector<ConnectionInfo>& current);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::ActiveConnectionsMonitor>,
              "ActiveConnectionsMonitor must not be copyable.");
