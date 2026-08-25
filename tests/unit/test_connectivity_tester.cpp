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
    void cancel_stopsPingLoopEarly();
    void ping_localhost_completesSuccessfully();
    void ping_clampsOutOfRangeCountBeforeLooping();

    // -- sanitizeConfig bounds (B9-17) -----------------------------
    void sanitizePing_clampsOutOfRange();
    void sanitizePing_passesValidThrough();
    void sanitizeTraceroute_clampsOutOfRange();
    void sanitizeMtr_clampsOutOfRange();
};

void TestConnectivityTester::construction_default() {
    ConnectivityTester tester;
    // The upcast to QObject* is compile-time non-null (ConnectivityTester : public QObject),
    // so it verifies nothing; pin the moc name to prove Q_OBJECT is present and namespaced.
    QCOMPARE(QByteArray(tester.metaObject()->className()),
             QByteArrayLiteral("sak::ConnectivityTester"));
}

void TestConnectivityTester::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<ConnectivityTester>);
    QVERIFY(!std::is_move_constructible_v<ConnectivityTester>);
}

void TestConnectivityTester::pingConfig_defaults() {
    ConnectivityTester::PingConfig config;
    QVERIFY(config.target.isEmpty());

    // Pin the shipped VALUES, not each constant against itself: comparing
    // config.count to netdiag::kDefaultPingCount merely restates the member
    // initialiser (connectivity_tester.h:28-32), so both sides move together.
    // traceroute/MTR pass kDefaultPingPacketSize to sendIcmpEcho unsanitized
    // (connectivity_tester.cpp:507, :572), so the literal values matter.
    QCOMPARE(config.count, 10);
    QCOMPARE(config.intervalMs, 1000);
    QCOMPARE(config.timeoutMs, 4000);
    QCOMPARE(config.packetSizeBytes, 32);
    QCOMPARE(config.ttl, 128);
    QVERIFY(config.resolveHostnames);

    // Every default must survive sanitizeConfig untouched. A default that drifts
    // outside its clamp (network_diagnostic_types.h:49-58) is silently rewritten
    // on every call (connectivity_tester.cpp:430, :399-408), so the shipped
    // default would not be the declared one -- e.g. ttl 0 ships as ttl 1 and each
    // echo expires at the first hop.
    const auto sanitized = ConnectivityTester::sanitizeConfig(config);
    QCOMPARE(sanitized.count, config.count);
    QCOMPARE(sanitized.intervalMs, config.intervalMs);
    QCOMPARE(sanitized.timeoutMs, config.timeoutMs);
    QCOMPARE(sanitized.packetSizeBytes, config.packetSizeBytes);
    QCOMPARE(sanitized.ttl, config.ttl);
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

void TestConnectivityTester::cancel_stopsPingLoopEarly() {
    ConnectivityTester tester;
    QSignalSpy complete_spy(&tester, &ConnectivityTester::pingComplete);

    // pingReply is emitted from inside the ping loop on this same thread
    // (connectivity_tester.cpp:459), so this slot runs synchronously and the
    // cancel lands before the loop-top check at :445.
    const QMetaObject::Connection cancelOnFirstReply = QObject::connect(
        &tester, &ConnectivityTester::pingReply, &tester, [&tester](const PingReply&) {
            tester.cancel();
        });

    ConnectivityTester::PingConfig config;
    config.target = QStringLiteral("127.0.0.1");
    config.count = 5;
    config.intervalMs = netdiag::kMinIntervalMs;
    config.timeoutMs = 2000;
    config.resolveHostnames = false;

    tester.ping(config);

    // sent == attempts actually made (:469), so a working cancel() stops the run
    // after the first reply instead of completing all five.
    QCOMPARE(complete_spy.count(), 1);
    const auto cancelled = complete_spy.takeFirst().at(0).value<PingResult>();
    QCOMPARE(cancelled.sent, 1);

    // ping() re-arms the flag at :429: the next run is not still cancelled.
    QObject::disconnect(cancelOnFirstReply);
    config.count = 3;
    tester.ping(config);

    QCOMPARE(complete_spy.count(), 1);
    const auto rearmed = complete_spy.takeFirst().at(0).value<PingResult>();
    QCOMPARE(rearmed.sent, 3);
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
    QCOMPARE(static_cast<int>(result.replies.size()), 3);

    // received is not a floor: it is exactly the number of replies flagged success, and every
    // reply carries its 1-based sequence number (connectivity_tester.cpp:451, :453-458).
    int successes = 0;
    for (int i = 0; i < static_cast<int>(result.replies.size()); ++i) {
        const PingReply& reply = result.replies.at(i);
        QCOMPARE(reply.sequenceNumber, i + 1);
        if (reply.success) {
            ++successes;
        }
    }
    QVERIFY(successes > 0);  // loopback must answer at least once
    QCOMPARE(result.received, successes);

    // Derived siblings the panel renders, computed in computePingStats
    // (connectivity_tester.cpp:75-77).
    QCOMPARE(result.lost, result.sent - successes);
    QCOMPARE(result.lossPercent,
             (static_cast<double>(result.sent - successes) / result.sent) * 100.0);

    QCOMPARE(result.resolvedIP, QStringLiteral("127.0.0.1"));
    QVERIFY(result.received > 0);
    QCOMPARE(result.resolvedIP, QStringLiteral("127.0.0.1"));
}
void TestConnectivityTester::ping_clampsOutOfRangeCountBeforeLooping() {
    ConnectivityTester tester;
    QSignalSpy complete_spy(&tester, &ConnectivityTester::pingComplete);

    ConnectivityTester::PingConfig config;
    config.target = QStringLiteral("127.0.0.1");
    config.count = 0;  // out of range: must clamp UP, not silently skip the loop
    config.timeoutMs = 2000;
    config.resolveHostnames = false;

    tester.ping(config);

    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.takeFirst().at(0).value<PingResult>();
    QCOMPARE(result.resolvedIP, QStringLiteral("127.0.0.1"));
    // Proves ping() actually runs its config through sanitizeConfig(): `sent` counts real
    // attempts, so a clamped count yields kMinPingCount attempts, while an unsanitised
    // count of 0 never enters the loop body and would leave sent == 0.
    QCOMPARE(result.sent, netdiag::kMinPingCount);
    QCOMPARE(result.replies.size(), static_cast<qsizetype>(netdiag::kMinPingCount));
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

    // Upper arms the "absurd" block above never reaches: intervalMs/timeoutMs are only
    // ever driven negative there, so their kMax ceilings would otherwise go unproved --
    // and packetSizeBytes is only ever driven high, leaving its floor unproved.
    ConnectivityTester::PingConfig high;
    high.intervalMs = 10'000'000;  // ~2.7h per gap at QThread::msleep (connectivity_tester.cpp:463)
    high.timeoutMs = 10'000'000;   // DWORD handed to IcmpSendEcho (connectivity_tester.cpp:382)
    high.packetSizeBytes = -1;     // negative -> the WORD cast would wrap to 65535
    const auto sh = ConnectivityTester::sanitizeConfig(high);
    QCOMPARE(sh.intervalMs, netdiag::kMaxIntervalMs);
    QCOMPARE(sh.timeoutMs, netdiag::kMaxPingTimeoutMs);
    QCOMPARE(sh.packetSizeBytes, netdiag::kMinPacketSizeBytes);
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
    // Upper arm: an absurd timeout is multiplied by the hop sweep -- up to kMaxHops (255)
    // hops x kMaxProbesPerHop (10) blocking IcmpSendEcho calls -- so it must cap at the max.
    ConnectivityTester::TracerouteConfig hi;
    hi.timeoutMs = 10'000'000;
    const auto sh = ConnectivityTester::sanitizeConfig(hi);
    QCOMPARE(sh.timeoutMs, netdiag::kMaxPingTimeoutMs);

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

    // Opposite arm of every clamp: the low end (and the interval's high end) must clamp too.
    ConnectivityTester::MtrConfig low;
    low.cycles = 0;              // zero/negative -> a run that never cycles
    low.intervalMs = 5'000'000;  // absurd -> an ~83-minute msleep between cycles
    low.maxHops = -5;            // negative -> QVector<MtrHopStats> of negative size
    low.timeoutMs = -1;          // negative -> huge DWORD timeout cast
    const auto sl = ConnectivityTester::sanitizeConfig(low);
    QCOMPARE(sl.cycles, netdiag::kMinMtrCycles);
    QCOMPARE(sl.intervalMs, netdiag::kMaxIntervalMs);
    QCOMPARE(sl.maxHops, netdiag::kMinHops);
    QCOMPARE(sl.timeoutMs, netdiag::kMinPingTimeoutMs);
}

QTEST_MAIN(TestConnectivityTester)
#include "test_connectivity_tester.moc"
