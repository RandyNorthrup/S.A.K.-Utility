// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_bandwidth_tester.cpp
/// @brief Unit tests for BandwidthTester

#include "sak/bandwidth_tester.h"
#include "sak/network_diagnostic_types.h"
#include "sak/network_transfer_runner.h"

#include "../support/tcp_listeners.h"
#include "../support/test_hardware_guard.h"

#include <QNetworkInterface>
#include <QProcess>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <algorithm>

#include <windows.h>

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
    void staleFirewallRules_onlyThoseNoLiveSakOwns();
    void iperfServer_runsInsideAKillOnCloseJob();
    void iperfServerArguments_bindOneAddressAndDropIdlePeers();
    void iperfServer_listensOnlyOnTheChosenAddress();
    void iperfServer_startsOnlyWithItsFirewallRule();
    void iperfServer_withoutAdministratorRefusesBeforeAnythingRuns();
    void iperfServer_needsAdministratorOnlyForARule();
    void firewallRuleRefusal_endsWithOneFullStopAndNamesNoCause();

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
    void networkTransfer_refusesToHoldAnInstallerSizedBodyInMemory();
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
    const bool anyCandidateOnDisk = std::ranges::any_of(candidates, [](const QString& candidate) {
        return QFileInfo::exists(candidate);
    });

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

// An installer of up to 4 GiB was once read whole into memory to be hashed. A transfer that may be
// larger than the in-memory ceiling must now stream to a sink: without one it is refused before
// any request is sent.
void TestBandwidthTester::networkTransfer_refusesToHoldAnInstallerSizedBodyInMemory() {
    NetworkTransferRequest request;
    request.url = QUrl(QStringLiteral("http://127.0.0.1:9/never-contacted"));
    request.timeout_ms = 1000;
    request.max_response_bytes = sak::kMaxInMemoryResponseBytes + 1;
    const NetworkTransferResult held = runNetworkTransfer(request);
    QVERIFY(!held.success);
    QVERIFY2(held.error_message.contains(QStringLiteral("must be streamed")),
             qPrintable(held.error_message));
    QCOMPARE(held.elapsed_ms, 0LL);

    // The same cap with a sink is allowed to try; it fails only on the closed port.
    request.body_sink = [](const char*, qint64) {
        return true;
    };
    const NetworkTransferResult streamed = runNetworkTransfer(request);
    QVERIFY(!streamed.success);
    QVERIFY2(!streamed.error_message.contains(QStringLiteral("must be streamed")),
             qPrintable(streamed.error_message));
}

// A crash left the iPerf3 server's inbound allow rule behind for good. Rule names now carry the
// owner's process ID, and a server start sweeps rules whose owner is gone -- never its own, never
// a live SAK's, never a rule that is not ours.
void TestBandwidthTester::staleFirewallRules_onlyThoseNoLiveSakOwns() {
    const QStringList names{
        QStringLiteral("SAK_Utility_iPerf3_5201_100-0123456789abcdef0123456789abcdef"),  // own
        QStringLiteral("SAK_Utility_iPerf3_5201_200-0123456789abcdef0123456789abcdef"),  // live
        QStringLiteral("SAK_Utility_iPerf3_5201_300-0123456789abcdef0123456789abcdef"),  // gone
        QStringLiteral("SAK_Utility_iPerf3_5201_0123456789abcdef0123456789abcdef"),  // pre-owner
        QStringLiteral("Some Other App iPerf3 rule"),
        QStringLiteral("SAK_Utility_iPerf3_5201_300-x; netsh delete")};
    const auto alive = [](qint64 pid) {
        return pid == 200;
    };
    QCOMPARE(BandwidthTester::staleFirewallRules(names, 100, alive),
             QStringList({names.at(2), names.at(3)}));
}

namespace {

QProcess* iperfServerProcessOf(const BandwidthTester& tester) {
    for (auto* child : tester.findChildren<QProcess*>()) {
        if (child->program().endsWith(QStringLiteral("iperf3.exe"), Qt::CaseInsensitive)) {
            return child;
        }
    }
    return nullptr;
}

// A non-loopback IPv4 address of this PC, so a server there would need an inbound rule; when the
// PC has none, a documentation address (TEST-NET-1) stands in: the rule is decided first.
QHostAddress nonLoopbackAddress() {
    for (const QHostAddress& address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() &&
            !address.isLinkLocal()) {
            return address;
        }
    }
    return QHostAddress(QStringLiteral("192.0.2.1"));
}

constexpr quint16 kBoundServerPort = 52'918;
constexpr quint16 kRuleServerPort = 52'919;
constexpr int kServerWaitMs = 10'000;

}  // namespace

// The server's command line names the one address it listens on and an idle limit, and nothing
// it is given can widen that: a wildcard or empty address is not a listen address at all.
void TestBandwidthTester::iperfServerArguments_bindOneAddressAndDropIdlePeers() {
    const QStringList args =
        BandwidthTester::iperfServerArguments(5201, QHostAddress(QStringLiteral("10.1.2.3")));
    const auto bind = args.indexOf(QStringLiteral("-B"));
    QVERIFY(bind >= 0 && bind + 1 < args.size());
    QCOMPARE(args.at(bind + 1), QStringLiteral("10.1.2.3"));
    const auto idle = args.indexOf(QStringLiteral("--idle-timeout"));
    QVERIFY(idle >= 0 && idle + 1 < args.size());
    QCOMPARE(args.at(idle + 1), QString::number(kIperfServerIdleTimeoutSec));
    QVERIFY(args.contains(QStringLiteral("-s")));

    QVERIFY(BandwidthTester::isSpecificListenAddress(QHostAddress(QStringLiteral("10.1.2.3"))));
    QVERIFY(BandwidthTester::isSpecificListenAddress(QHostAddress(QHostAddress::LocalHost)));
    QVERIFY(!BandwidthTester::isSpecificListenAddress(QHostAddress()));
    QVERIFY(!BandwidthTester::isSpecificListenAddress(QHostAddress(QHostAddress::Any)));
    QVERIFY(!BandwidthTester::isSpecificListenAddress(QHostAddress(QHostAddress::AnyIPv4)));
    QVERIFY(!BandwidthTester::isSpecificListenAddress(QHostAddress(QHostAddress::AnyIPv6)));
    QVERIFY(!BandwidthTester::isSpecificListenAddress(QHostAddress(QHostAddress::Broadcast)));

    BandwidthTester tester;
    QSignalSpy errors(&tester, &BandwidthTester::errorOccurred);
    tester.startIperfServer(kBoundServerPort, QHostAddress(QHostAddress::Any));
    QCOMPARE(errors.count(), 1);
    QVERIFY(iperfServerProcessOf(tester) == nullptr);
}

// iperf3 listened on every address the PC has. Bound to loopback here (no inbound rule needed,
// so the test hardware guard's refusal of firewall changes does not stop it), Windows must show
// the server listening on 127.0.0.1 alone.
void TestBandwidthTester::iperfServer_listensOnlyOnTheChosenAddress() {
    BandwidthTester tester;
    // A server only this PC can reach needs no rule, so no administrator either.
    tester.setElevatedForTesting(false);
    QVERIFY(tester.isIperf3Available());
    QSignalSpy started(&tester, &BandwidthTester::serverStarted);
    tester.startIperfServer(kBoundServerPort, QHostAddress(QHostAddress::LocalHost));
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, kServerWaitMs);
    QProcess* server = iperfServerProcessOf(tester);
    QVERIFY(server != nullptr);
    const auto pid = static_cast<DWORD>(server->processId());
    QTRY_VERIFY_WITH_TIMEOUT(!sak::test::tcpListenersOf(pid, kBoundServerPort).isEmpty(),
                             kServerWaitMs);
    const QStringList listeners = sak::test::tcpListenersOf(pid);
    tester.stopIperfServer();
    QTRY_VERIFY_WITH_TIMEOUT(!tester.isServerRunning(), kServerWaitMs);
    QCOMPARE(listeners, QStringList{QStringLiteral("127.0.0.1:%1").arg(kBoundServerPort)});
}

// A server other PCs can reach starts only once its inbound firewall rule is in place. Without
// the rule it used to start anyway, so Windows asked the user whether to let iperf3.exe through
// (on public networks too). This runs as an elevated S.A.K. would (no real elevation): the rule is
// attempted, and here the test hardware guard refuses it, as Windows refuses a standard user's
// netsh in CI. The server must not start, and the error must give netsh's reason and no cause of
// its own: S.A.K. was elevated, so "needs administrator" would be a guess. Where a test may
// change the firewall and the rule is added, the server starts bound to that address alone.
void TestBandwidthTester::iperfServer_startsOnlyWithItsFirewallRule() {
    BandwidthTester tester;
    tester.setElevatedForTesting(true);
    QVERIFY(tester.isIperf3Available());
    QSignalSpy started(&tester, &BandwidthTester::serverStarted);
    QSignalSpy errors(&tester, &BandwidthTester::errorOccurred);
    const QHostAddress address = nonLoopbackAddress();
    tester.startIperfServer(kRuleServerPort, address);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() > 0 || errors.count() > 0, kServerWaitMs);
    const QString said = errors.isEmpty() ? QString() : errors.first().at(0).toString();
    const QString lead = QStringLiteral(
        "iPerf3 server not started: Windows Firewall did not add its inbound rule: ");
    const bool refused = said.startsWith(lead);
    if (!sak::test::realHardwareAllowed()) {
        QVERIFY2(refused, said.isEmpty() ? "no error" : qPrintable(said));
    }
    if (refused) {
        QVERIFY2(said.size() > lead.size() + 1, qPrintable(said));
        QVERIFY2(!said.contains(QStringLiteral("Run as Admin to")) &&
                     !said.contains(QStringLiteral("needs S.A.K.")) &&
                     !said.contains(QStringLiteral("Adding it needs")),
                 qPrintable(said));
        QTest::qWait(500);  // nothing may start late either
        QCOMPARE(started.count(), 0);
        QVERIFY(iperfServerProcessOf(tester) == nullptr);
        QVERIFY(!tester.isServerRunning());
        return;
    }
    QProcess* server = iperfServerProcessOf(tester);
    QVERIFY(server != nullptr);
    const auto pid = static_cast<DWORD>(server->processId());
    QTRY_VERIFY_WITH_TIMEOUT(!sak::test::tcpListenersOf(pid, kRuleServerPort).isEmpty(),
                             kServerWaitMs);
    const QStringList listeners = sak::test::tcpListenersOf(pid);
    tester.stopIperfServer();
    QTRY_VERIFY_WITH_TIMEOUT(!tester.isServerRunning(), kServerWaitMs);
    QCOMPARE(listeners,
             QStringList{QStringLiteral("%1:%2").arg(address.toString()).arg(kRuleServerPort)});
}

// Not elevated, a server other PCs can reach is refused before anything runs: no stale-rule
// sweep, no server process, no netsh. The message leads with what to do, because the status bar
// shows it beside its own Run as Admin button and cut the old one off at "S.A".
void TestBandwidthTester::iperfServer_withoutAdministratorRefusesBeforeAnythingRuns() {
    BandwidthTester tester;
    tester.setElevatedForTesting(false);
    QVERIFY(tester.isIperf3Available());
    QSignalSpy started(&tester, &BandwidthTester::serverStarted);
    QSignalSpy errors(&tester, &BandwidthTester::errorOccurred);
    tester.startIperfServer(kRuleServerPort, nonLoopbackAddress());
    QCOMPARE(errors.count(), 1);  // at once, not after a netsh run
    const QString said = errors.first().at(0).toString();
    QCOMPARE(said, BandwidthTester::needsElevationMessage());
    QVERIFY2(said.startsWith(QStringLiteral("Run as Admin to start the iPerf3 server")),
             qPrintable(said));
    QVERIFY2(tester.findChildren<QProcess*>().isEmpty(), "a process was started before refusing");
    QTest::qWait(500);  // nothing may start late either
    QCOMPARE(started.count(), 0);
    QCOMPARE(errors.count(), 1);
    QVERIFY(!tester.isServerRunning());
}

// Only a server other PCs can reach needs administrator rights: loopback needs no rule, IPv4 or
// IPv6, elevated or not.
void TestBandwidthTester::iperfServer_needsAdministratorOnlyForARule() {
    const QHostAddress lan(QStringLiteral("192.168.1.20"));
    QVERIFY(BandwidthTester::serverNeedsElevation(lan, false));
    QVERIFY(!BandwidthTester::serverNeedsElevation(lan, true));
    for (const auto& loopback :
         {QHostAddress(QHostAddress::LocalHost), QHostAddress(QHostAddress::LocalHostIPv6)}) {
        QVERIFY(!BandwidthTester::serverNeedsElevation(loopback, false));
        QVERIFY(!BandwidthTester::serverNeedsElevation(loopback, true));
    }
}

// netsh ends its own sentence, and its elevation refusal ends in a bracket: the old message gave
// "(... (Run as administrator).). Adding it needs S.A.K. running as administrator." Now netsh's
// words are the last clause and the message ends with one full stop whatever they end with.
void TestBandwidthTester::firewallRuleRefusal_endsWithOneFullStopAndNamesNoCause() {
    const QString lead =
        QStringLiteral("iPerf3 server not started: Windows Firewall did not add its inbound rule");
    QCOMPARE(BandwidthTester::firewallRuleRefusalMessage(QStringLiteral(
                 "The requested operation requires elevation (Run as administrator).")),
             lead + QStringLiteral(": The requested operation requires elevation (Run as "
                                   "administrator)."));
    QCOMPARE(BandwidthTester::firewallRuleRefusalMessage(QStringLiteral("Access is denied")),
             lead + QStringLiteral(": Access is denied."));
    QCOMPARE(BandwidthTester::firewallRuleRefusalMessage(
                 QStringLiteral("  Group policy\r\n  blocks it... ")),
             lead + QStringLiteral(": Group policy blocks it."));
    QCOMPARE(BandwidthTester::firewallRuleRefusalMessage(QStringLiteral("Failed!")),
             lead + QStringLiteral(": Failed."));
    QCOMPARE(BandwidthTester::firewallRuleRefusalMessage(QString()), lead + QLatin1Char('.'));
    QCOMPARE(BandwidthTester::firewallRuleRefusalMessage(QStringLiteral(" . ")),
             lead + QLatin1Char('.'));
    for (const QString& why :
         {QStringLiteral("x."), QStringLiteral("(y)."), QStringLiteral("z;")}) {
        const QString message = BandwidthTester::firewallRuleRefusalMessage(why);
        QVERIFY2(!message.contains(QStringLiteral("..")) &&
                     !message.contains(QStringLiteral(".).")) &&
                     !message.contains(QStringLiteral(";.")),
                 qPrintable(message));
        QVERIFY2(!message.contains(QStringLiteral("needs S.A.K.")), qPrintable(message));
    }
}

// The server used to be a plain QProcess outside any Job, so a crash of this app left iperf3
// listening. It now starts inside a kill-on-close Job.
void TestBandwidthTester::iperfServer_runsInsideAKillOnCloseJob() {
    BandwidthTester tester;
    if (!tester.isIperf3Available()) {
        // No bundled tool, no server to contain: starting must refuse out loud instead.
        QSignalSpy errors(&tester, &BandwidthTester::errorOccurred);
        tester.startIperfServer(52'917, QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(errors.count(), 1);
        QVERIFY(!tester.isServerRunning());
        return;
    }
    QSignalSpy started(&tester, &BandwidthTester::serverStarted);
    tester.startIperfServer(52'917, QHostAddress(QHostAddress::LocalHost));
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10'000);
    QProcess* server = nullptr;  // netsh and the rule sweep are QProcess children too
    for (auto* child : tester.findChildren<QProcess*>()) {
        if (child->program().endsWith(QStringLiteral("iperf3.exe"), Qt::CaseInsensitive)) {
            server = child;
        }
    }
    QVERIFY(server != nullptr);
    // The server's QProcess owns the Job that contains it (and so outlives it by nothing).
    QVERIFY2(server->findChild<QObject*>(QStringLiteral("iperf3ServerJob")) != nullptr,
             "the iPerf3 server must be launched inside its own Job");
    BOOL self_in_job = FALSE;
    QVERIFY(IsProcessInJob(GetCurrentProcess(), nullptr, &self_in_job));
    if (self_in_job) {
        // Under CTest this process is itself in a Job, so the OS-level membership check below
        // would pass for any child; the ownership check above is what holds here.
        tester.stopIperfServer();
        QTRY_VERIFY_WITH_TIMEOUT(!tester.isServerRunning(), 10'000);
        return;
    }
    const qint64 pid = server->processId();
    QVERIFY(pid > 0);
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    QVERIFY(handle != nullptr);
    BOOL in_job = FALSE;
    const BOOL queried = IsProcessInJob(handle, nullptr, &in_job);
    CloseHandle(handle);
    QVERIFY(queried);
    QVERIFY2(in_job, "the iPerf3 server must run inside a Job");
    tester.stopIperfServer();
    QTRY_VERIFY_WITH_TIMEOUT(!tester.isServerRunning(), 10'000);
}

QTEST_MAIN(TestBandwidthTester)
#include "test_bandwidth_tester.moc"
