// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_controller.cpp
/// @brief Unit tests for NetworkDiagnosticController

#include "sak/network_diagnostic_controller.h"

#include <QSignalSpy>
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
    void generateReport_unknownFormat_failsClosed();
    void runLanTransferTest_durationOverMax_failsClosed();
    void runLanTransferTest_blockSizeOverMax_failsClosed();
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

QTEST_MAIN(TestNetworkDiagnosticController)
#include "test_network_diagnostic_controller.moc"
