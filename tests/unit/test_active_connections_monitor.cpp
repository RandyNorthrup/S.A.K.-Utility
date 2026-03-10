// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_active_connections_monitor.cpp
/// @brief Unit tests for ActiveConnectionsMonitor

#include "sak/active_connections_monitor.h"
#include "sak/network_diagnostic_types.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace sak;

class TestActiveConnectionsMonitor : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void monitorConfig_defaults();
    void monitorConfig_fieldAssignment();
    void connectionInfo_defaults();
    void currentConnections_emptyInitially();
    void stopMonitoring_noStart();
    void startMonitoring_emitsConnectionsUpdated();
    void startStop_lifecycle();
};

void TestActiveConnectionsMonitor::construction_default() {
    ActiveConnectionsMonitor monitor;
    QVERIFY(dynamic_cast<QObject*>(&monitor) != nullptr);
}

void TestActiveConnectionsMonitor::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<ActiveConnectionsMonitor>);
    QVERIFY(!std::is_move_constructible_v<ActiveConnectionsMonitor>);
}

void TestActiveConnectionsMonitor::monitorConfig_defaults() {
    ActiveConnectionsMonitor::MonitorConfig config;
    QCOMPARE(config.refreshIntervalMs, netdiag::kDefaultConnRefreshMs);
    QVERIFY(!config.resolveHostnames);
    QVERIFY(config.resolveProcessNames);
    QVERIFY(config.showTcp);
    QVERIFY(config.showUdp);
    QVERIFY(config.filterProcessName.isEmpty());
    QCOMPARE(config.filterPort, static_cast<uint16_t>(0));
}

void TestActiveConnectionsMonitor::monitorConfig_fieldAssignment() {
    ActiveConnectionsMonitor::MonitorConfig config;
    config.refreshIntervalMs = 500;
    config.resolveHostnames = true;
    config.resolveProcessNames = false;
    config.showTcp = false;
    config.showUdp = true;
    config.filterProcessName = QStringLiteral("svchost.exe");
    config.filterPort = 443;

    QCOMPARE(config.refreshIntervalMs, 500);
    QVERIFY(config.resolveHostnames);
    QVERIFY(!config.resolveProcessNames);
    QVERIFY(!config.showTcp);
    QVERIFY(config.showUdp);
    QCOMPARE(config.filterProcessName, QStringLiteral("svchost.exe"));
    QCOMPARE(config.filterPort, static_cast<uint16_t>(443));
}

void TestActiveConnectionsMonitor::connectionInfo_defaults() {
    ConnectionInfo info;
    QCOMPARE(info.protocol, ConnectionInfo::Protocol::TCP);
    QVERIFY(info.localAddress.isEmpty());
    QCOMPARE(info.localPort, static_cast<uint16_t>(0));
    QVERIFY(info.remoteAddress.isEmpty());
    QCOMPARE(info.remotePort, static_cast<uint16_t>(0));
    QVERIFY(info.state.isEmpty());
    QCOMPARE(info.processId, 0u);
    QVERIFY(info.processName.isEmpty());
}

void TestActiveConnectionsMonitor::currentConnections_emptyInitially() {
    ActiveConnectionsMonitor monitor;
    const auto connections = monitor.getCurrentConnections();
    QVERIFY(connections.isEmpty());
}

void TestActiveConnectionsMonitor::stopMonitoring_noStart() {
    ActiveConnectionsMonitor monitor;
    monitor.stopMonitoring();
    QVERIFY(monitor.getCurrentConnections().isEmpty());
}

void TestActiveConnectionsMonitor::startMonitoring_emitsConnectionsUpdated() {
    ActiveConnectionsMonitor monitor;
    QSignalSpy updated_spy(&monitor, &ActiveConnectionsMonitor::connectionsUpdated);

    ActiveConnectionsMonitor::MonitorConfig config;
    config.refreshIntervalMs = 100;
    config.resolveHostnames = false;
    config.resolveProcessNames = false;

    monitor.startMonitoring(config);

    constexpr int kMaxWaitMs = 3000;
    QVERIFY(updated_spy.wait(kMaxWaitMs));
    QVERIFY(updated_spy.count() >= 1);

    monitor.stopMonitoring();
}

void TestActiveConnectionsMonitor::startStop_lifecycle() {
    ActiveConnectionsMonitor monitor;

    ActiveConnectionsMonitor::MonitorConfig config;
    config.refreshIntervalMs = 100;
    config.resolveHostnames = false;
    config.resolveProcessNames = false;

    monitor.startMonitoring(config);

    QSignalSpy updated_spy(&monitor, &ActiveConnectionsMonitor::connectionsUpdated);
    QVERIFY(updated_spy.wait(3000));

    const auto connections = monitor.getCurrentConnections();
    QVERIFY2(!connections.isEmpty(), "Running system should have active TCP/UDP connections");

    monitor.stopMonitoring();

    QVERIFY(monitor.getCurrentConnections().size() >= 0);
}

QTEST_MAIN(TestActiveConnectionsMonitor)
#include "test_active_connections_monitor.moc"
