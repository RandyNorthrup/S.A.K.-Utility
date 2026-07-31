// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_controller.cpp
/// @brief Unit tests for DiagnosticController suite state machine, skip guard, and error handling

#include "sak/diagnostic_controller.h"

#include <QtTest/QtTest>

using namespace sak;

class DiagnosticControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initialState();
    void suiteStateEnum();
    void cancelCurrentResetsState();
    void thermalMonitorAccess();
    void reportDataAccess();
    // B5-14: stale-completion guard and fail-open aggregate status.
    void suiteAdvanceAllowed_onlyForCurrentStep();
    void statusWithStepFailures_failClosed();
};

void DiagnosticControllerTests::initialState() {
    DiagnosticController controller;

    QCOMPARE(controller.currentState(), DiagnosticController::SuiteState::Idle);
}

void DiagnosticControllerTests::suiteStateEnum() {
    // Verify all suite states are distinct values
    QVERIFY(DiagnosticController::SuiteState::Idle !=
            DiagnosticController::SuiteState::HardwareScan);
    QVERIFY(DiagnosticController::SuiteState::HardwareScan !=
            DiagnosticController::SuiteState::SmartAnalysis);
    QVERIFY(DiagnosticController::SuiteState::SmartAnalysis !=
            DiagnosticController::SuiteState::CpuBenchmark);
    QVERIFY(DiagnosticController::SuiteState::CpuBenchmark !=
            DiagnosticController::SuiteState::DiskBenchmark);
    QVERIFY(DiagnosticController::SuiteState::DiskBenchmark !=
            DiagnosticController::SuiteState::MemoryBenchmark);
    QVERIFY(DiagnosticController::SuiteState::MemoryBenchmark !=
            DiagnosticController::SuiteState::StressTest);
    QVERIFY(DiagnosticController::SuiteState::StressTest !=
            DiagnosticController::SuiteState::ReportGeneration);
    QVERIFY(DiagnosticController::SuiteState::ReportGeneration !=
            DiagnosticController::SuiteState::Complete);
}

void DiagnosticControllerTests::cancelCurrentResetsState() {
    DiagnosticController controller;
    QSignalSpy state_spy(&controller, &DiagnosticController::suiteStateChanged);

    // Cancel when idle should still emit Idle state
    controller.cancelCurrent();

    QVERIFY(state_spy.count() >= 1);
    const auto last_state = state_spy.last()[0].value<DiagnosticController::SuiteState>();
    QCOMPARE(last_state, DiagnosticController::SuiteState::Idle);
    QCOMPARE(controller.currentState(), DiagnosticController::SuiteState::Idle);
}

void DiagnosticControllerTests::thermalMonitorAccess() {
    DiagnosticController controller;
    auto* monitor = controller.thermalMonitor();
    QVERIFY(monitor != nullptr);
}

void DiagnosticControllerTests::reportDataAccess() {
    DiagnosticController controller;
    const auto& data = controller.reportData();
    QCOMPARE(data.overall_status, DiagnosticStatus::AllPassed);
    QVERIFY(data.smart_reports.isEmpty());
    QVERIFY(!data.cpu_benchmark.has_value());
}

// B5-14: only the worker for the step currently in progress may advance the
// suite. A stale completion (cancelled/skipped worker, or skip's own queued
// advance) that no longer matches the current step must be ignored, preventing
// a double-advance that would silently skip the following step.
void DiagnosticControllerTests::suiteAdvanceAllowed_onlyForCurrentStep() {
    using S = DiagnosticController::SuiteState;
    // Running + step matches the current state -> advance.
    QVERIFY(DiagnosticController::suiteAdvanceAllowed(true, S::CpuBenchmark, S::CpuBenchmark));
    // Running but the completing step is NOT the current one -> stale, ignore.
    QVERIFY(!DiagnosticController::suiteAdvanceAllowed(true, S::DiskBenchmark, S::CpuBenchmark));
    // Not running -> never advance, even on a match.
    QVERIFY(!DiagnosticController::suiteAdvanceAllowed(false, S::CpuBenchmark, S::CpuBenchmark));
}

// B5-14: a suite with a failed step must not report AllPassed; a critical
// finding is never downgraded to a mere warning by the same rule.
void DiagnosticControllerTests::statusWithStepFailures_failClosed() {
    // No failures -> status unchanged.
    QCOMPARE(DiagnosticController::statusWithStepFailures(DiagnosticStatus::AllPassed, false),
             DiagnosticStatus::AllPassed);
    // A failed step turns AllPassed into Warnings.
    QCOMPARE(DiagnosticController::statusWithStepFailures(DiagnosticStatus::AllPassed, true),
             DiagnosticStatus::Warnings);
    // Never upgrades: an existing critical status stays critical.
    QCOMPARE(DiagnosticController::statusWithStepFailures(DiagnosticStatus::CriticalIssues, true),
             DiagnosticStatus::CriticalIssues);
    // An existing warning stays a warning.
    QCOMPARE(DiagnosticController::statusWithStepFailures(DiagnosticStatus::Warnings, true),
             DiagnosticStatus::Warnings);
}

QTEST_MAIN(DiagnosticControllerTests)
#include "test_diagnostic_controller.moc"
