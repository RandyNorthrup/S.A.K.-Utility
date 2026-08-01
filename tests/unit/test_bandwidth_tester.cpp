// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_bandwidth_tester.cpp
/// @brief Unit tests for BandwidthTester

#include "sak/bandwidth_tester.h"
#include "sak/network_diagnostic_types.h"

#include <QtTest/QtTest>

using namespace sak;

class TestBandwidthTester : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void iperfConfig_defaults();
    void iperfConfig_fieldAssignment();
    void serverNotRunning_initially();
    void cancel_doesNotCrash();
    void stopServer_withoutStart();
    void isIperf3Available_returnsBool();
    void bandwidthTestResult_defaults();

    // ── composeFirewallRuleName (per-instance uniqueness, B9-16) ──
    void firewallRuleName_containsPortAndToken();
    void firewallRuleName_uniquePerToken();
    void firewallRuleName_noShellMetacharacters();

    // ── parseIperfJson (bad-output not a success, B9-18) ──────────
    void parseIperf_rejectsGarbage();
    void parseIperf_rejectsJsonWithoutEndSummary();
    void parseIperf_acceptsValidTcpResult();
};

void TestBandwidthTester::construction_default() {
    BandwidthTester tester;
    QVERIFY(dynamic_cast<QObject*>(&tester) != nullptr);
    QVERIFY(!tester.isServerRunning());
}

void TestBandwidthTester::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<BandwidthTester>);
    QVERIFY(!std::is_move_constructible_v<BandwidthTester>);
}

void TestBandwidthTester::iperfConfig_defaults() {
    BandwidthTester::IperfConfig config;
    QVERIFY(config.serverAddress.isEmpty());
    QCOMPARE(config.port, netdiag::kDefaultIperfPort);
    QCOMPARE(config.durationSec, netdiag::kDefaultBandwidthDuration);
    QCOMPARE(config.parallelStreams, 1);
    QVERIFY(config.bidirectional);
    QVERIFY(!config.udpMode);
    QCOMPARE(config.udpBandwidthMbps, 100);
}

void TestBandwidthTester::iperfConfig_fieldAssignment() {
    BandwidthTester::IperfConfig config;
    config.serverAddress = QStringLiteral("192.168.1.100");
    config.port = 5202;
    config.durationSec = 30;
    config.parallelStreams = 4;
    config.bidirectional = false;
    config.udpMode = true;
    config.udpBandwidthMbps = 500;

    QCOMPARE(config.serverAddress, QStringLiteral("192.168.1.100"));
    QCOMPARE(config.port, static_cast<uint16_t>(5202));
    QCOMPARE(config.durationSec, 30);
    QCOMPARE(config.parallelStreams, 4);
    QVERIFY(!config.bidirectional);
    QVERIFY(config.udpMode);
    QCOMPARE(config.udpBandwidthMbps, 500);
}

void TestBandwidthTester::serverNotRunning_initially() {
    BandwidthTester tester;
    QVERIFY(!tester.isServerRunning());
}

void TestBandwidthTester::cancel_doesNotCrash() {
    BandwidthTester tester;
    tester.cancel();
    QVERIFY(!tester.isServerRunning());
}

void TestBandwidthTester::stopServer_withoutStart() {
    BandwidthTester tester;
    tester.stopIperfServer();
    QVERIFY(!tester.isServerRunning());
}

void TestBandwidthTester::isIperf3Available_returnsBool() {
    BandwidthTester tester;
    const bool available = tester.isIperf3Available();
    Q_UNUSED(available);
    // Verify it returns without crashing; result depends on tools/ dir
    QVERIFY(!tester.isServerRunning());
}

void TestBandwidthTester::bandwidthTestResult_defaults() {
    BandwidthTestResult result;
    QCOMPARE(result.mode, BandwidthTestResult::TestMode::LanIperf3);
    QVERIFY(result.target.isEmpty());
    QCOMPARE(result.downloadMbps, 0.0);
    QCOMPARE(result.uploadMbps, 0.0);
    QCOMPARE(result.retransmissions, 0.0);
    QCOMPARE(result.jitterMs, 0.0);
    QCOMPARE(result.packetLossPercent, 0.0);
    QVERIFY(result.intervals.isEmpty());
    QCOMPARE(result.durationSec, 0);
    QCOMPARE(result.parallelStreams, 0);
    QVERIFY(!result.reverseMode);
}

// ═══════════════════════════════════════════════════════════════════
// composeFirewallRuleName -- per-instance uniqueness (B9-16)
// ═══════════════════════════════════════════════════════════════════

void TestBandwidthTester::firewallRuleName_containsPortAndToken() {
    const QString name = BandwidthTester::composeFirewallRuleName(5201, QStringLiteral("abc123"));
    QVERIFY(name.contains(QStringLiteral("5201")));
    QVERIFY(name.contains(QStringLiteral("abc123")));
    // Same inputs -> stable name (so create and remove match).
    QCOMPARE(name, BandwidthTester::composeFirewallRuleName(5201, QStringLiteral("abc123")));
}

void TestBandwidthTester::firewallRuleName_uniquePerToken() {
    // Two concurrent servers get distinct tokens => distinct rule names, so one
    // stopping cannot delete the other's inbound rule.
    const QString a = BandwidthTester::composeFirewallRuleName(5201, QStringLiteral("token-A"));
    const QString b = BandwidthTester::composeFirewallRuleName(5201, QStringLiteral("token-B"));
    QVERIFY(a != b);
}

void TestBandwidthTester::firewallRuleName_noShellMetacharacters() {
    // netsh runs shell-free via argv, but keep the composed name free of quotes and
    // whitespace regardless.
    const QString name = BandwidthTester::composeFirewallRuleName(
        5201, QStringLiteral("0123456789abcdef0123456789abcdef"));
    QVERIFY(!name.contains(QLatin1Char('"')));
    QVERIFY(!name.contains(QLatin1Char(' ')));
}

// ═══════════════════════════════════════════════════════════════════
// parseIperfJson -- unparseable output must not read as a zero success (B9-18)
// ═══════════════════════════════════════════════════════════════════

void TestBandwidthTester::parseIperf_rejectsGarbage() {
    QVERIFY(!BandwidthTester::parseIperfJson(QByteArrayLiteral("not json at all")).has_value());
    QVERIFY(!BandwidthTester::parseIperfJson(QByteArray()).has_value());
}

void TestBandwidthTester::parseIperf_rejectsJsonWithoutEndSummary() {
    // Valid JSON, but not an iPerf3 result (no `end` throughput summary): must be
    // rejected rather than yielding an all-zero "success".
    const auto r = BandwidthTester::parseIperfJson(QByteArrayLiteral("{\"start\":{}}"));
    QVERIFY(!r.has_value());
}

void TestBandwidthTester::parseIperf_acceptsValidTcpResult() {
    const QByteArray json = QByteArrayLiteral(
        "{\"end\":{\"sum_sent\":{\"bits_per_second\":100000000,\"retransmits\":2},"
        "\"sum_received\":{\"bits_per_second\":90000000}}}");
    const auto r = BandwidthTester::parseIperfJson(json);
    QVERIFY(r.has_value());
    QCOMPARE(r->uploadMbps, 100.0);   // 100 Mbit/s
    QCOMPARE(r->downloadMbps, 90.0);  // 90 Mbit/s
    QCOMPARE(r->retransmissions, 2.0);
}

QTEST_MAIN(TestBandwidthTester)
#include "test_bandwidth_tester.moc"
