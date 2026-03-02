// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_controller.cpp
/// @brief Unit tests for DiagnosticController suite state machine, skip guard, and error handling

#include <QtTest/QtTest>

#include "sak/diagnostic_controller.h"

using namespace sak;

class DiagnosticControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initialState();
    void suiteStateEnum();
    void cancelCurrentResetsState();
    void thermalMonitorAccess();
    void reportDataAccess();
};

void DiagnosticControllerTests::initialState()
{
    DiagnosticController controller;

    QCOMPARE(controller.currentState(), DiagnosticController::SuiteState::Idle);
}

void DiagnosticControllerTests::suiteStateEnum()
{
    // Verify all suite states are distinct values
    QVERIFY(
        DiagnosticController::SuiteState::Idle != DiagnosticController::SuiteState::HardwareScan);
    QVERIFY(
        DiagnosticController::SuiteState::HardwareScan
            != DiagnosticController::SuiteState::SmartAnalysis);
    QVERIFY(
        DiagnosticController::SuiteState::SmartAnalysis
            != DiagnosticController::SuiteState::CpuBenchmark);
    QVERIFY(
        DiagnosticController::SuiteState::CpuBenchmark
            != DiagnosticController::SuiteState::DiskBenchmark);
    QVERIFY(
        DiagnosticController::SuiteState::DiskBenchmark
            != DiagnosticController::SuiteState::MemoryBenchmark);
    QVERIFY(
        DiagnosticController::SuiteState::MemoryBenchmark
            != DiagnosticController::SuiteState::StressTest);
    QVERIFY(
        DiagnosticController::SuiteState::StressTest
            != DiagnosticController::SuiteState::ReportGeneration);
    QVERIFY(
        DiagnosticController::SuiteState::ReportGeneration
            != DiagnosticController::SuiteState::Complete);
}

void DiagnosticControllerTests::cancelCurrentResetsState()
{
    DiagnosticController controller;
    QSignalSpy state_spy(&controller, &DiagnosticController::suiteStateChanged);

    // Cancel when idle should still emit Idle state
    controller.cancelCurrent();

    QVERIFY(state_spy.count() >= 1);
    const auto last_state = state_spy.last()[0].value<DiagnosticController::SuiteState>();
    QCOMPARE(last_state, DiagnosticController::SuiteState::Idle);
    QCOMPARE(controller.currentState(), DiagnosticController::SuiteState::Idle);
}

void DiagnosticControllerTests::thermalMonitorAccess()
{
    DiagnosticController controller;
    auto* monitor = controller.thermalMonitor();
    QVERIFY(monitor != nullptr);
}

void DiagnosticControllerTests::reportDataAccess()
{
    DiagnosticController controller;
    const auto& data = controller.reportData();
    QCOMPARE(data.overall_status, DiagnosticStatus::AllPassed);
    QVERIFY(data.smart_reports.isEmpty());
    QVERIFY(!data.cpu_benchmark.has_value());
}

QTEST_MAIN(DiagnosticControllerTests)
#include "test_diagnostic_controller.moc"
