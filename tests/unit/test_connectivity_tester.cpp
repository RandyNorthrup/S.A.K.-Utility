// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_connectivity_tester.cpp
/// @brief Unit tests for ConnectivityTester

#include "sak/connectivity_tester.h"
#include "sak/network_diagnostic_types.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace sak;

class TestConnectivityTester : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void pingConfig_defaults();
    void pingConfig_fieldAssignment();
    void tracerouteConfig_defaults();
    void mtrConfig_defaults();
    void pingReply_defaults();
    void pingResult_defaults();
    void tracerouteHop_defaults();
    void mtrHopStats_defaults();
    void cancel_doesNotCrash();
    void ping_localhost_completesSuccessfully();

    // -- sanitizeConfig bounds (B9-17) -----------------------------
    void sanitizePing_clampsOutOfRange();
    void sanitizePing_passesValidThrough();
    void sanitizeTraceroute_clampsOutOfRange();
    void sanitizeMtr_clampsOutOfRange();
};

void TestConnectivityTester::construction_default() {
    ConnectivityTester tester;
    QVERIFY(dynamic_cast<QObject*>(&tester) != nullptr);
}

void TestConnectivityTester::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<ConnectivityTester>);
    QVERIFY(!std::is_move_constructible_v<ConnectivityTester>);
}

void TestConnectivityTester::pingConfig_defaults() {
    ConnectivityTester::PingConfig config;
    QVERIFY(config.target.isEmpty());
    QCOMPARE(config.count, netdiag::kDefaultPingCount);
    QCOMPARE(config.intervalMs, netdiag::kDefaultPingIntervalMs);
    QCOMPARE(config.timeoutMs, netdiag::kDefaultPingTimeoutMs);
    QCOMPARE(config.packetSizeBytes, netdiag::kDefaultPingPacketSize);
    QCOMPARE(config.ttl, netdiag::kDefaultPingTtl);
    QVERIFY(config.resolveHostnames);
}

void TestConnectivityTester::pingConfig_fieldAssignment() {
    ConnectivityTester::PingConfig config;
    config.target = QStringLiteral("127.0.0.1");
    config.count = 5;
    config.intervalMs = 500;
    config.timeoutMs = 2000;
    config.packetSizeBytes = 64;
    config.ttl = 64;
    config.resolveHostnames = false;

    QCOMPARE(config.target, QStringLiteral("127.0.0.1"));
    QCOMPARE(config.count, 5);
    QCOMPARE(config.intervalMs, 500);
    QCOMPARE(config.timeoutMs, 2000);
    QCOMPARE(config.packetSizeBytes, 64);
    QCOMPARE(config.ttl, 64);
    QVERIFY(!config.resolveHostnames);
}

void TestConnectivityTester::tracerouteConfig_defaults() {
    ConnectivityTester::TracerouteConfig config;
    QVERIFY(config.target.isEmpty());
    QCOMPARE(config.maxHops, netdiag::kDefaultTracerouteMaxHops);
    QCOMPARE(config.timeoutMs, netdiag::kDefaultTracerouteTimeout);
    QCOMPARE(config.probesPerHop, netdiag::kDefaultTracerouteProbes);
    QVERIFY(config.resolveHostnames);
}

void TestConnectivityTester::mtrConfig_defaults() {
    ConnectivityTester::MtrConfig config;
    QVERIFY(config.target.isEmpty());
    QCOMPARE(config.cycles, netdiag::kDefaultMtrCycles);
    QCOMPARE(config.intervalMs, netdiag::kDefaultPingIntervalMs);
    QCOMPARE(config.maxHops, netdiag::kDefaultTracerouteMaxHops);
    QCOMPARE(config.timeoutMs, netdiag::kDefaultTracerouteTimeout);
}

void TestConnectivityTester::cancel_doesNotCrash() {
    ConnectivityTester tester;
    tester.cancel();
    QVERIFY(dynamic_cast<QObject*>(&tester) != nullptr);
}

void TestConnectivityTester::pingReply_defaults() {
    PingReply reply;
    QCOMPARE(reply.sequenceNumber, 0);
    QVERIFY(!reply.success);
    QCOMPARE(reply.rttMs, 0.0);
    QCOMPARE(reply.ttl, 0);
    QVERIFY(reply.replyFrom.isEmpty());
    QVERIFY(reply.errorMessage.isEmpty());
}

void TestConnectivityTester::pingResult_defaults() {
    PingResult result;
    QVERIFY(result.target.isEmpty());
    QVERIFY(result.resolvedIP.isEmpty());
    QVERIFY(result.replies.isEmpty());
    QCOMPARE(result.sent, 0);
    QCOMPARE(result.received, 0);
    QCOMPARE(result.lost, 0);
    QCOMPARE(result.lossPercent, 0.0);
    QCOMPARE(result.minRtt, 0.0);
    QCOMPARE(result.maxRtt, 0.0);
    QCOMPARE(result.avgRtt, 0.0);
    QCOMPARE(result.jitter, 0.0);
}

void TestConnectivityTester::tracerouteHop_defaults() {
    TracerouteHop hop;
    QCOMPARE(hop.hopNumber, 0);
    QVERIFY(hop.ipAddress.isEmpty());
    QVERIFY(hop.hostname.isEmpty());
    QCOMPARE(hop.rtt1Ms, 0.0);
    QCOMPARE(hop.rtt2Ms, 0.0);
    QCOMPARE(hop.rtt3Ms, 0.0);
    QCOMPARE(hop.avgRttMs, 0.0);
    QVERIFY(!hop.timedOut);
}

void TestConnectivityTester::mtrHopStats_defaults() {
    MtrHopStats stats;
    QCOMPARE(stats.hopNumber, 0);
    QVERIFY(stats.ipAddress.isEmpty());
    QCOMPARE(stats.sent, 0);
    QCOMPARE(stats.received, 0);
    QCOMPARE(stats.lossPercent, 0.0);
    QCOMPARE(stats.lastRttMs, 0.0);
    QCOMPARE(stats.avgRttMs, 0.0);
    QCOMPARE(stats.bestRttMs, 0.0);
    QCOMPARE(stats.worstRttMs, 0.0);
    QCOMPARE(stats.jitterMs, 0.0);
}

void TestConnectivityTester::ping_localhost_completesSuccessfully() {
    ConnectivityTester tester;
    QSignalSpy complete_spy(&tester, &ConnectivityTester::pingComplete);

    ConnectivityTester::PingConfig config;
    config.target = QStringLiteral("127.0.0.1");
    config.count = 3;
    config.intervalMs = 100;
    config.timeoutMs = 2000;
    config.resolveHostnames = false;

    tester.ping(config);

    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.takeFirst().at(0).value<PingResult>();
    QCOMPARE(result.sent, 3);
    QVERIFY(result.received > 0);
    QCOMPARE(result.resolvedIP, QStringLiteral("127.0.0.1"));
}

// ===================================================================
// sanitizeConfig -- clamp every numeric field to a safe range (B9-17)
// ===================================================================

void TestConnectivityTester::sanitizePing_clampsOutOfRange() {
    ConnectivityTester::PingConfig config;
    config.count = 5'000'000;            // absurd -> loop cap
    config.intervalMs = -100;            // negative -> would be a huge unsigned msleep
    config.timeoutMs = -1;               // negative -> huge DWORD
    config.packetSizeBytes = 1'000'000;  // overflows the WORD cast + giant alloc
    config.ttl = 9999;                   // overflows the UCHAR cast

    const auto s = ConnectivityTester::sanitizeConfig(config);
    QCOMPARE(s.count, netdiag::kMaxPingCount);
    QCOMPARE(s.intervalMs, netdiag::kMinIntervalMs);
    QCOMPARE(s.timeoutMs, netdiag::kMinPingTimeoutMs);
    QCOMPARE(s.packetSizeBytes, netdiag::kMaxPacketSizeBytes);
    QCOMPARE(s.ttl, netdiag::kMaxTtl);

    // Low end: zero/negative clamps up to the minimums.
    ConnectivityTester::PingConfig low;
    low.count = 0;
    low.ttl = 0;
    const auto sl = ConnectivityTester::sanitizeConfig(low);
    QCOMPARE(sl.count, netdiag::kMinPingCount);
    QCOMPARE(sl.ttl, netdiag::kMinTtl);
}

void TestConnectivityTester::sanitizePing_passesValidThrough() {
    ConnectivityTester::PingConfig config;
    config.count = 5;
    config.intervalMs = 500;
    config.timeoutMs = 2000;
    config.packetSizeBytes = 64;
    config.ttl = 64;

    const auto s = ConnectivityTester::sanitizeConfig(config);
    QCOMPARE(s.count, 5);
    QCOMPARE(s.intervalMs, 500);
    QCOMPARE(s.timeoutMs, 2000);
    QCOMPARE(s.packetSizeBytes, 64);
    QCOMPARE(s.ttl, 64);
}

void TestConnectivityTester::sanitizeTraceroute_clampsOutOfRange() {
    ConnectivityTester::TracerouteConfig config;
    config.maxHops = 9999;
    config.timeoutMs = -1;
    config.probesPerHop = 500;
    const auto s = ConnectivityTester::sanitizeConfig(config);
    QCOMPARE(s.maxHops, netdiag::kMaxHops);
    QCOMPARE(s.timeoutMs, netdiag::kMinPingTimeoutMs);
    QCOMPARE(s.probesPerHop, netdiag::kMaxProbesPerHop);

    ConnectivityTester::TracerouteConfig low;
    low.maxHops = 0;
    low.probesPerHop = 0;
    const auto sl = ConnectivityTester::sanitizeConfig(low);
    QCOMPARE(sl.maxHops, netdiag::kMinHops);
    QCOMPARE(sl.probesPerHop, netdiag::kMinProbesPerHop);
}

void TestConnectivityTester::sanitizeMtr_clampsOutOfRange() {
    ConnectivityTester::MtrConfig config;
    config.cycles = 5'000'000;
    config.intervalMs = -50;
    config.maxHops = 9999;
    config.timeoutMs = 10'000'000;
    const auto s = ConnectivityTester::sanitizeConfig(config);
    QCOMPARE(s.cycles, netdiag::kMaxMtrCycles);
    QCOMPARE(s.intervalMs, netdiag::kMinIntervalMs);
    QCOMPARE(s.maxHops, netdiag::kMaxHops);
    QCOMPARE(s.timeoutMs, netdiag::kMaxPingTimeoutMs);
}

QTEST_MAIN(TestConnectivityTester)
#include "test_connectivity_tester.moc"
