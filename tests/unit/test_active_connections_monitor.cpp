// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_active_connections_monitor.cpp
/// @brief Unit tests for ActiveConnectionsMonitor

#include "sak/active_connections_monitor.h"
#include "sak/network_diagnostic_types.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QtTest/QtTest>

#include <algorithm>

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
    // startMonitoring() calls refreshNow() synchronously, so the spy already holds one
    // emission by the time it returns. The original wait() therefore blocked for a SECOND
    // emission, i.e. it was testing that the 100ms refresh timer actually fires -- so the
    // assertion has to be >= 2. Settling for >= 1 here would pass on the synchronous emit
    // alone and stop covering the timer entirely.
    QTRY_VERIFY_WITH_TIMEOUT(updated_spy.count() >= 2, kMaxWaitMs);

    monitor.stopMonitoring();
}

void TestActiveConnectionsMonitor::startStop_lifecycle() {
    ActiveConnectionsMonitor monitor;

    // Open a real loopback TCP listener owned by THIS test process, so the enumeration
    // has a deterministic connection to find. The prior assertion -- "a running system
    // should have active TCP/UDP connections" -- was environment-dependent (G18-5): an
    // isolated CI runner or a quiet host can legitimately have zero connections at the
    // sampled instant, so it passed or failed by accident of the machine, not the code.
    // A LISTEN socket we open ourselves is returned by
    // GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL) regardless of host activity, so the
    // non-empty claim now genuinely tests that enumeration surfaces a known connection.
    QTcpServer server;
    QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
             qPrintable(QStringLiteral("loopback listen failed: %1").arg(server.errorString())));
    const quint16 knownPort = server.serverPort();

    ActiveConnectionsMonitor::MonitorConfig config;
    config.refreshIntervalMs = 100;
    config.resolveHostnames = false;
    config.resolveProcessNames = false;

    monitor.startMonitoring(config);

    // Unlike the test above, this spy is constructed AFTER startMonitoring(), so it misses
    // the synchronous first refresh and >= 1 is the timer-driven emission.
    QSignalSpy updated_spy(&monitor, &ActiveConnectionsMonitor::connectionsUpdated);
    QTRY_VERIFY_WITH_TIMEOUT(updated_spy.count() >= 1, 3000);

    const auto connections = monitor.getCurrentConnections();
    const bool foundKnownListener =
        std::ranges::any_of(connections, [knownPort](const ConnectionInfo& c) {
            return c.protocol == ConnectionInfo::Protocol::TCP && c.localPort == knownPort;
        });
    QVERIFY2(foundKnownListener,
             "Enumeration must surface this process's own loopback TCP listener");

    monitor.stopMonitoring();

    // Was QVERIFY(size() >= 0), which is a tautology: size() returns qsizetype and can
    // never be negative, so it asserted nothing. The real claim is that stopping only stops
    // the refresh timer and leaves the last snapshot readable, so a caller that stops
    // monitoring can still render what it already had.
    QCOMPARE(monitor.getCurrentConnections().size(), connections.size());
}

QTEST_MAIN(TestActiveConnectionsMonitor)
#include "test_active_connections_monitor.moc"
