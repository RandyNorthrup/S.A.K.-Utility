// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_controller.cpp
/// @brief Unit tests for NetworkDiagnosticController

#include "sak/network_diagnostic_controller.h"

#include <QtTest/QtTest>

using namespace sak;

class TestNetworkDiagnosticController : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void initialState_idle();
    void cancel_doesNotCrash();
    void iperfServer_notRunningInitially();
    void lanTransferServer_notRunningInitially();
};

void TestNetworkDiagnosticController::construction_default() {
    NetworkDiagnosticController controller;
    QVERIFY(dynamic_cast<QObject*>(&controller) != nullptr);
}

void TestNetworkDiagnosticController::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<NetworkDiagnosticController>);
    QVERIFY(!std::is_move_constructible_v<NetworkDiagnosticController>);
}

void TestNetworkDiagnosticController::initialState_idle() {
    NetworkDiagnosticController controller;
    QCOMPARE(controller.currentState(), NetworkDiagnosticController::State::Idle);
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

QTEST_MAIN(TestNetworkDiagnosticController)
#include "test_network_diagnostic_controller.moc"
