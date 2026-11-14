// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_controller.cpp
/// @brief Unit tests for NetworkDiagnosticController

#include "sak/network_diagnostic_controller.h"

#include "../support/tcp_listeners.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;

class TestNetworkDiagnosticController : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void initialState_idle();
    void initialState_noActiveOps();
    void cancel_doesNotCrash();
    void iperfServer_notRunningInitially();
    void lanTransferServer_notRunningInitially();
    void lanTransferServer_listensOnlyOnTheChosenAddress();
    void serverListenChoices_preferRememberedThenDefaultRoute();
    void generateReport_unknownFormat_failsClosed();
    void generateReport_includesAFinishedHttpSpeedTest();
    void generateReport_omitsAStoppedHttpSpeedTest();
    void logLines_neverStateASpeedThatWasNotMeasured();
    void runLanTransferTest_durationOverMax_failsClosed();
    void runLanTransferTest_blockSizeOverMax_failsClosed();
    void continuousWiFiScan_firstScanRunsOffTheGuiThread();
};

void TestNetworkDiagnosticController::construction_default() {
    NetworkDiagnosticController controller;
    // The vacuous upcast can never be null; pin the moc metaobject name instead, proving
    // Q_OBJECT is present and correctly namespaced (a missing macro would break its signals).
    QCOMPARE(QByteArray(controller.metaObject()->className()),
             QByteArrayLiteral("sak::NetworkDiagnosticController"));
}

void TestNetworkDiagnosticController::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<NetworkDiagnosticController>);
    QVERIFY(!std::is_move_constructible_v<NetworkDiagnosticController>);
}

void TestNetworkDiagnosticController::initialState_idle() {
    NetworkDiagnosticController controller;
    QCOMPARE(controller.currentState(), NetworkDiagnosticController::State::Idle);
}

void TestNetworkDiagnosticController::initialState_noActiveOps() {
    NetworkDiagnosticController controller;
    QVERIFY(!controller.hasActiveOperations());
    QVERIFY(!controller.isOperationActive(NetworkDiagnosticController::State::RunningPing));
}

void TestNetworkDiagnosticController::cancel_doesNotCrash() {
    NetworkDiagnosticController controller;
    controller.cancel();
    QCOMPARE(controller.currentState(), NetworkDiagnosticController::State::Idle);
}

void TestNetworkDiagnosticController::iperfServer_notRunningInitially() {
    NetworkDiagnosticController controller;
    QVERIFY(!controller.isIperfServerRunning());
}

void TestNetworkDiagnosticController::lanTransferServer_notRunningInitially() {
    NetworkDiagnosticController controller;
    QVERIFY(!controller.isLanTransferServerRunning());
}

// The LAN transfer server listened on QHostAddress::Any: every network the PC was on, public
// ones included. It now listens on the one address it is given, and refuses a wildcard.
void TestNetworkDiagnosticController::lanTransferServer_listensOnlyOnTheChosenAddress() {
    NetworkDiagnosticController controller;
    QSignalSpy errors(&controller, &NetworkDiagnosticController::errorOccurred);
    controller.startLanTransferServer(0, QHostAddress(QHostAddress::Any));
    QCOMPARE(errors.count(), 1);
    QVERIFY(!controller.isLanTransferServerRunning());

    QSignalSpy started(&controller, &NetworkDiagnosticController::lanTransferServerStarted);
    controller.startLanTransferServer(0, QHostAddress(QHostAddress::LocalHost));
    QCOMPARE(started.count(), 1);
    const auto port = started.first().at(0).value<uint16_t>();
    QVERIFY(port != 0);
    const QStringList listeners = sak::test::tcpListenersOf(GetCurrentProcessId(), port);
    controller.stopLanTransferServer();
    QCOMPARE(listeners, QStringList{QStringLiteral("127.0.0.1:%1").arg(port)});
}

// The "Listen on" list: each connected adapter's IPv4 addresses, the default route's adapter
// first in preference, and the address a server last used kept when it is still there.
void TestNetworkDiagnosticController::serverListenChoices_preferRememberedThenDefaultRoute() {
    NetworkAdapterInfo vpn;
    vpn.name = QStringLiteral("VPN");
    vpn.isConnected = true;
    vpn.ipv4Addresses = {QStringLiteral("10.8.0.2")};
    NetworkAdapterInfo ethernet;
    ethernet.name = QStringLiteral("Ethernet");
    ethernet.isConnected = true;
    ethernet.ipv4Addresses = {QStringLiteral("192.168.1.20")};
    ethernet.ipv4Gateway = QStringLiteral("192.168.1.1");
    NetworkAdapterInfo unplugged;
    unplugged.name = QStringLiteral("Wi-Fi");
    unplugged.isConnected = false;
    unplugged.ipv4Addresses = {QStringLiteral("192.168.50.9")};
    unplugged.ipv4Gateway = QStringLiteral("192.168.50.1");

    using Controller = NetworkDiagnosticController;
    const auto choices = Controller::serverListenChoices({vpn, ethernet, unplugged});
    QCOMPARE(choices.size(), 2);
    QCOMPARE(choices.at(0).address, QStringLiteral("10.8.0.2"));
    QCOMPARE(choices.at(1).label, QStringLiteral("Ethernet - 192.168.1.20"));
    QVERIFY(!choices.at(0).default_route);
    QVERIFY(choices.at(1).default_route);

    QCOMPARE(Controller::preferredListenChoice(choices, QString()), 1);
    QCOMPARE(Controller::preferredListenChoice(choices, QStringLiteral("10.8.0.2")), 0);
    QCOMPARE(Controller::preferredListenChoice(choices, QStringLiteral("192.168.50.9")), 1);
    QCOMPARE(Controller::preferredListenChoice({}, QStringLiteral("10.8.0.2")), -1);
}

void TestNetworkDiagnosticController::generateReport_unknownFormat_failsClosed() {
    // Codex-3: an unrecognized format must fail closed (surface an error), not silently
    // fall back to producing an HTML report.
    NetworkDiagnosticController controller;
    QSignalSpy failed(&controller, &NetworkDiagnosticController::errorOccurred);
    QSignalSpy done(&controller, &NetworkDiagnosticController::reportGenerated);

    controller.generateReport(
        QStringLiteral("out.xyz"), QStringLiteral("xml"), QString(), QString(), QString());

    QCOMPARE(failed.count(), 1);
    QCOMPARE(done.count(), 0);
    QVERIFY(!controller.hasActiveOperations());
    QCOMPARE(controller.currentState(), NetworkDiagnosticController::State::Idle);
}

namespace {
constexpr int kReportTimeoutMs = 30'000;

QJsonObject bandwidthSectionOf(NetworkDiagnosticController& controller, const QString& path) {
    QSignalSpy done(&controller, &NetworkDiagnosticController::reportGenerated);
    controller.generateReport(path, QStringLiteral("json"), QString(), QString(), QString());
    if (!done.wait(kReportTimeoutMs)) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll())
        .object()
        .value(QStringLiteral("bandwidth"))
        .toObject();
}
}  // namespace

// The report listed a finished speed test as not run: only iPerf3's result was ever kept for it.
void TestNetworkDiagnosticController::generateReport_includesAFinishedHttpSpeedTest() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    NetworkDiagnosticController controller;

    Q_EMIT controller.httpSpeedComplete(171.31, 26.98, 227.0);

    const QJsonObject bandwidth = bandwidthSectionOf(controller,
                                                     dir.filePath(QStringLiteral("report.json")));
    QCOMPARE(bandwidth.value(QStringLiteral("mode")).toString(), QStringLiteral("wan-http"));
    QCOMPARE(bandwidth.value(QStringLiteral("target")).toString(),
             QStringLiteral("speed.cloudflare.com"));
    QCOMPARE(bandwidth.value(QStringLiteral("downloadMbps")).toDouble(), 171.31);
    QCOMPARE(bandwidth.value(QStringLiteral("uploadMbps")).toDouble(), 26.98);
    QCOMPARE(bandwidth.value(QStringLiteral("latencyMs")).toDouble(), 227.0);
}

// Stop and failure both complete with zeros, which must never reach a report as a measured speed.
void TestNetworkDiagnosticController::generateReport_omitsAStoppedHttpSpeedTest() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    NetworkDiagnosticController controller;

    Q_EMIT controller.httpSpeedComplete(0.0, 0.0, 0.0);

    QVERIFY(bandwidthSectionOf(controller, dir.filePath(QStringLiteral("report.json"))).isEmpty());
}

void TestNetworkDiagnosticController::logLines_neverStateASpeedThatWasNotMeasured() {
    using C = NetworkDiagnosticController;
    QCOMPARE(C::httpSpeedLogLine(171.31, 26.98, 227.0),
             QStringLiteral("HTTP speed test: DL 171.31 Mbps, UL 26.98 Mbps, latency 227.0 ms"));
    QCOMPARE(C::httpSpeedLogLine(171.31, 26.98, -1.0),
             QStringLiteral("HTTP speed test: DL 171.31 Mbps, UL 26.98 Mbps, latency unknown"));
    QCOMPARE(C::httpSpeedLogLine(0.0, 0.0, 0.0),
             QStringLiteral("HTTP speed test ended without a result"));

    BandwidthTestResult result;
    QCOMPARE(C::iperfLogLine(result), QStringLiteral("Bandwidth test ended without a result"));
    result.downloadMbps = 940.25;
    result.uploadMbps = 912.5;
    QCOMPARE(C::iperfLogLine(result),
             QStringLiteral("Bandwidth test: DL 940.25 Mbps, UL 912.50 Mbps"));
}

void TestNetworkDiagnosticController::runLanTransferTest_durationOverMax_failsClosed() {
    // Codex-3: an out-of-range duration must fail closed rather than be coerced or overflow
    // the int duration-ms math. No worker thread must be started.
    NetworkDiagnosticController controller;
    QSignalSpy failed(&controller, &NetworkDiagnosticController::errorOccurred);

    controller.runLanTransferTest(QStringLiteral("192.168.0.1"), 5201, 10'000'000, 64);

    QCOMPARE(failed.count(), 1);
    QVERIFY(!controller.hasActiveOperations());
}

void TestNetworkDiagnosticController::runLanTransferTest_blockSizeOverMax_failsClosed() {
    // Codex-3: an out-of-range block size must fail closed rather than overflow the int
    // block-buffer / socket-queue math. No worker thread must be started.
    NetworkDiagnosticController controller;
    QSignalSpy failed(&controller, &NetworkDiagnosticController::errorOccurred);

    controller.runLanTransferTest(QStringLiteral("192.168.0.1"), 5201, 10, 10'000'000);

    QCOMPARE(failed.count(), 1);
    QVERIFY(!controller.hasActiveOperations());
}

// The continuous scan's first pass is a full WlanScan plus a wait for the driver to signal
// completion. It ran inline in startContinuousWiFiScan -- on the GUI thread -- so its result was
// already delivered when the call returned. It now runs on the worker like scanWiFi(), so the
// result arrives later, through the event loop.
void TestNetworkDiagnosticController::continuousWiFiScan_firstScanRunsOffTheGuiThread() {
    sak::NetworkDiagnosticController controller;
    QSignalSpy scans(&controller, &sak::NetworkDiagnosticController::wifiScanComplete);
    QVERIFY(scans.isValid());
    constexpr int kLongIntervalMs = 600'000;
    controller.startContinuousWiFiScan(kLongIntervalMs);
    QCOMPARE(scans.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(scans.count() >= 1, 60'000);
    controller.stopContinuousWiFiScan();
}

QTEST_MAIN(TestNetworkDiagnosticController)
#include "test_network_diagnostic_controller.moc"
