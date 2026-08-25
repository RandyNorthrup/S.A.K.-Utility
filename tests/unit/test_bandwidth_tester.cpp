// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_bandwidth_tester.cpp
/// @brief Unit tests for BandwidthTester

#include "sak/bandwidth_tester.h"
#include "sak/network_diagnostic_types.h"
#include "sak/network_transfer_runner.h"

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
    void isIperf3Available_matchesBundledToolOnDisk();
    void bandwidthTestResult_defaults();

    // -- composeFirewallRuleName (per-instance uniqueness, B9-16) --
    void firewallRuleName_containsPortAndToken();
    void firewallRuleName_uniquePerToken();
    void firewallRuleName_noShellMetacharacters();

    // -- parseIperfJson (bad-output not a success, B9-18) ----------
    void parseIperf_rejectsGarbage();
    void parseIperf_rejectsJsonWithoutEndSummary();
    void parseIperf_acceptsValidTcpResult();
    void parseIperf_acceptsUdpResultFromEndSum();

    // -- composeNetshPath (absolute netsh, fail closed on empty root) --
    void netshPath_absoluteUnderSystemRoot();
    void netshPath_emptyRootFailsClosed();

    // -- runNetworkTransfer (R5-P9-34: a non-positive timeout is refused) --
    void networkTransfer_rejectsZeroTimeoutBeforeAnyRequest();
    void networkTransfer_rejectsNegativeTimeout();
};

void TestBandwidthTester::construction_default() {
    BandwidthTester tester;
    static_assert(std::is_base_of_v<QObject, BandwidthTester>);  // was a vacuous runtime upcast
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

void TestBandwidthTester::isIperf3Available_matchesBundledToolOnDisk() {
    // isIperf3Available() is not an "either": it is exactly "did findIperf3Path resolve one
    // of its three fixed candidates under applicationDirPath". Recompute that answer here,
    // independently of the function under test, instead of discarding it via Q_UNUSED.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString bundled = appDir + QStringLiteral("/tools/iperf3/iperf3.exe");
    const QStringList candidates = {
        bundled,
        appDir + QStringLiteral("/../tools/iperf3/iperf3.exe"),
        appDir + QStringLiteral("/iperf3.exe"),
    };
    bool anyCandidateOnDisk = false;
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            anyCandidateOnDisk = true;
            break;
        }
    }

    BandwidthTester tester;
    QCOMPARE(tester.isIperf3Available(), anyCandidateOnDisk);

    // The Release test binary is emitted into the app output dir (tests/CMakeLists.txt:22,
    // :6646), where the sak_utility POST_BUILD tools-bundle copy (CMakeLists.txt:1460-1468)
    // deploys tools/iperf3/iperf3.exe. Whenever that bundle is on disk the answer must be a
    // definite yes: dropping that candidate from findIperf3Path would make startIperfServer
    // (bandwidth_tester.cpp:273) and runIperfTest (:429) report "iPerf3 not found" for a
    // perfectly deployed tool. Scoped to the file's presence so the check is inert, never
    // wrong, in a layout that has no deployed bundle.
    if (QFileInfo::exists(bundled)) {
        QVERIFY2(tester.isIperf3Available(), qPrintable(bundled));
    }
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
    // tcpWindowSize has NO writer anywhere in the codebase -- parseIperfJson
    // (bandwidth_tester.cpp:518) default-constructs its result and never assigns it
    // (the ":528 Parse TCP window size" comment is stale), so the header's = 0.0 is
    // the field's only value and it rides the SUCCESS path to the panel via :484/:492.
    QCOMPARE(result.tcpWindowSize, 0.0);

    // A failed run must not render as data: runIperfTest sets timestamp ONLY on the
    // success path (bandwidth_tester.cpp:490), so the default must stay null or every
    // testComplete({}) failure exit (:431/:437/:451/:456/:463/:471/:481) would carry a
    // fresh, real-looking time.
    QVERIFY(!result.timestamp.isValid());

    // Pin the emission form used by those failure exits, not just default-init:
    // testComplete({}) value-initializes, and that zero/null result IS the contract.
    QCOMPARE(BandwidthTestResult{}.tcpWindowSize, 0.0);
    QVERIFY(!BandwidthTestResult{}.timestamp.isValid());
}

// ===================================================================
// composeFirewallRuleName -- per-instance uniqueness (B9-16)
// ===================================================================

void TestBandwidthTester::firewallRuleName_containsPortAndToken() {
    const QString name = BandwidthTester::composeFirewallRuleName(5201, QStringLiteral("abc123"));
    QCOMPARE(name, QStringLiteral("SAK_Utility_iPerf3_5201_abc123"));
    // Same inputs -> stable name (so create and remove match).
    QCOMPARE(name, BandwidthTester::composeFirewallRuleName(5201, QStringLiteral("abc123")));

    // The CALLER-supplied port must reach the name -- not netdiag::kDefaultIperfPort.
    // startIperfServer() feeds the same port to this name and to netsh "localport="
    // (bandwidth_tester.cpp:291-292, :700), so a name pinned only at the default value
    // cannot tell a real port-routed name from a hardcoded default (B9-16 cross-deletion).
    QCOMPARE(BandwidthTester::composeFirewallRuleName(5202, QStringLiteral("abc123")),
             QStringLiteral("SAK_Utility_iPerf3_5202_abc123"));
    // Distinct ports (same token) must yield distinct rule names.
    QVERIFY(BandwidthTester::composeFirewallRuleName(5202, QStringLiteral("abc123")) !=
            BandwidthTester::composeFirewallRuleName(5201, QStringLiteral("abc123")));
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
    // Exact composed name subsumes (and exceeds) the no-quote/no-space checks.
    QCOMPARE(name, QStringLiteral("SAK_Utility_iPerf3_5201_0123456789abcdef0123456789abcdef"));
}

// ===================================================================
// parseIperfJson -- unparseable output must not read as a zero success (B9-18)
// ===================================================================

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
void TestBandwidthTester::parseIperf_acceptsUdpResultFromEndSum() {
    // A UDP run (`iperf3 -u -J`) carries throughput ONLY in end.sum -- it has no
    // sum_sent/sum_received. Two things must hold or B9-18 is violated for UDP:
    //   (a) the third arm of the hasThroughput gate (bandwidth_tester.cpp:513) must
    //       ACCEPT this shape, else a successful UDP test is reported as
    //       "iPerf3 completed but returned unparseable output" (bandwidth_tester.cpp:479);
    //   (b) the UDP field block (bandwidth_tester.cpp:549-558) must fill
    //       upload/download/jitter/loss, else the run reads as a bogus all-zero
    //       "success" because lines 524-526 find no sum_sent/sum_received.
    const QByteArray json = QByteArrayLiteral(
        "{\"end\":{\"sum\":{\"bits_per_second\":50000000,\"jitter_ms\":1.5,"
        "\"lost_percent\":2.5}}}");
    const auto r = BandwidthTester::parseIperfJson(json);
    QVERIFY(r.has_value());
    // UDP throughput is symmetric: the single end.sum figure feeds both directions.
    QCOMPARE(r->uploadMbps, 50.0);    // 50 Mbit/s
    QCOMPARE(r->downloadMbps, 50.0);  // 50 Mbit/s
    QCOMPARE(r->jitterMs, 1.5);
    QCOMPARE(r->packetLossPercent, 2.5);
    // No TCP retransmit counter exists on a UDP run.
    QCOMPARE(r->retransmissions, 0.0);
}


// ===================================================================
// composeNetshPath -- absolute path only, fail closed on empty root
// ===================================================================

void TestBandwidthTester::netshPath_absoluteUnderSystemRoot() {
    // The privileged firewall calls must resolve netsh under System32, never a bare
    // "netsh" that a PATH/CWD-planted binary could hijack.
    const QString path = BandwidthTester::composeNetshPath(QStringLiteral("C:/Windows"));
    QCOMPARE(path, QStringLiteral("C:/Windows/System32/netsh.exe"));
    // Backslash-style root resolves to the same cleaned path.
    const QString back = BandwidthTester::composeNetshPath(QStringLiteral("C:\\Windows"));
    QCOMPARE(back, QStringLiteral("C:/Windows/System32/netsh.exe"));
}

void TestBandwidthTester::netshPath_emptyRootFailsClosed() {
    // No known Windows root -> no trusted netsh -> empty means "do not run" (fail
    // closed), never a fallback to bare "netsh".
    QVERIFY(BandwidthTester::composeNetshPath(QString()).isEmpty());
}

// ===================================================================
// runNetworkTransfer -- a non-positive timeout must be refused, never
// coerced to a default and never treated as "no timeout" (R5-P9-34)
// ===================================================================

void TestBandwidthTester::networkTransfer_rejectsZeroTimeoutBeforeAnyRequest() {
    // timeout_ms == 0 disables Qt's transfer timeout AND leaves the deadline timer unarmed, so
    // the synchronous runNetworkTransfer() would wait on a stalled server forever when no
    // cancel callback is supplied. The request must be refused before any network access: an
    // untouched elapsed_ms proves the worker thread never ran.
    NetworkTransferRequest request;
    request.url = QUrl(QStringLiteral("http://127.0.0.1:9/never-contacted"));
    request.timeout_ms = 0;

    const NetworkTransferResult result = runNetworkTransfer(request);
    QVERIFY(!result.success);
    QVERIFY(!result.timed_out);
    QCOMPARE(result.error_message, QStringLiteral("Invalid timeout_ms: must be positive"));
    QCOMPARE(result.elapsed_ms, 0LL);
    QCOMPARE(result.bytes_received, 0LL);
    QVERIFY(result.body.isEmpty());
}

void TestBandwidthTester::networkTransfer_rejectsNegativeTimeout() {
    NetworkTransferRequest request;
    request.url = QUrl(QStringLiteral("http://127.0.0.1:9/never-contacted"));
    request.timeout_ms = -1;

    const NetworkTransferResult result = runNetworkTransfer(request);
    QVERIFY(!result.success);
    QCOMPARE(result.error_message, QStringLiteral("Invalid timeout_ms: must be positive"));
    QCOMPARE(result.elapsed_ms, 0LL);
}

QTEST_MAIN(TestBandwidthTester)
#include "test_bandwidth_tester.moc"
