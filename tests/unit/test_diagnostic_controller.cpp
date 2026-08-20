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
    void reportDataAccess();
    void thermalReadingsAreForwardedToTheController();
    // B5-14: stale-completion guard and fail-open aggregate status.
    void suiteAdvanceAllowed_onlyForCurrentStep();
    void statusWithStepFailures_failClosed();

    // B5-16: report format require-output + collision-free names.
    void requestedReportFormats_requiresKnownFormat();
    void uniqueReportBaseName_neverCollides();
};

void DiagnosticControllerTests::initialState() {
    DiagnosticController controller;

    QCOMPARE(controller.currentState(), DiagnosticController::SuiteState::Idle);
}

void DiagnosticControllerTests::suiteStateEnum() {
    // The ordinal is indexed in production: onSuiteStateChanged does static_cast<int>(state) into
    // kStateToStep (std::array<int,9>), so a reorder silently maps each state to the wrong step.
    // Pin the exact ordinals, not just adjacent-pair distinctness (a language guarantee).
    using S = DiagnosticController::SuiteState;
    QCOMPARE(static_cast<int>(S::Idle), 0);
    QCOMPARE(static_cast<int>(S::HardwareScan), 1);
    QCOMPARE(static_cast<int>(S::SmartAnalysis), 2);
    QCOMPARE(static_cast<int>(S::CpuBenchmark), 3);
    QCOMPARE(static_cast<int>(S::DiskBenchmark), 4);
    QCOMPARE(static_cast<int>(S::MemoryBenchmark), 5);
    QCOMPARE(static_cast<int>(S::StressTest), 6);
    QCOMPARE(static_cast<int>(S::ReportGeneration), 7);
    QCOMPARE(static_cast<int>(S::Complete), 8);
}

void DiagnosticControllerTests::cancelCurrentResetsState() {
    DiagnosticController controller;
    QSignalSpy state_spy(&controller, &DiagnosticController::suiteStateChanged);

    // Cancel when idle should still emit Idle state
    controller.cancelCurrent();

    QCOMPARE(state_spy.count(), 1);  // one synchronous emit; no worker running to fire more
    const auto last_state = state_spy.last()[0].value<DiagnosticController::SuiteState>();
    QCOMPARE(last_state, DiagnosticController::SuiteState::Idle);
    QCOMPARE(controller.currentState(), DiagnosticController::SuiteState::Idle);
}

// The controller owns the ThermalMonitor and forwards its readings as its own
// signal; that forwarding is the ONLY way a panel sees live temperatures. This
// replaces an older test that merely asserted a thermalMonitor() getter returned
// non-null -- which proved the object existed but not that anything reached the
// caller, and which existed only for that test.
//
// processReadings emits unconditionally (an empty reading set from an
// unprivileged sensor query is still a result), so this does not depend on the
// box having readable sensors.
void DiagnosticControllerTests::thermalReadingsAreForwardedToTheController() {
    DiagnosticController controller;
    QSignalSpy spy(&controller, &DiagnosticController::thermalReadingsUpdated);
    QVERIFY(spy.isValid());

    controller.startThermalMonitoring(500);
    // A poll spawns an interpreter, so allow the same generous bound
    // test_thermal_monitor uses; it normally arrives in a couple of seconds.
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 30'000);
    controller.stopThermalMonitoring();
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

// B5-16: an empty/unknown format spec must produce NO output and must not be
// reported as success. requestedReportFormats is what generateReport requires
// to be non-empty.
void DiagnosticControllerTests::requestedReportFormats_requiresKnownFormat() {
    // Known formats are recognized, case-insensitively, in html/json/csv order.
    const auto all = DiagnosticController::requestedReportFormats("HTML,json,Csv");
    QCOMPARE(all, QStringList({"html", "json", "csv"}));

    const auto one = DiagnosticController::requestedReportFormats("json");
    QCOMPARE(one, QStringList({"json"}));

    // No known format -> empty, so generateReport refuses instead of "succeeding".
    QVERIFY(DiagnosticController::requestedReportFormats("").isEmpty());
    QVERIFY(DiagnosticController::requestedReportFormats("pdf").isEmpty());
    QVERIFY(DiagnosticController::requestedReportFormats("   ").isEmpty());
}

// B5-16: two reports produced in the same second must not share a base name.
void DiagnosticControllerTests::uniqueReportBaseName_neverCollides() {
    const QDateTime when = QDateTime::fromString("2026-07-30T12:00:00.500", Qt::ISODateWithMs);
    const QString a = DiagnosticController::uniqueReportBaseName("C:/out", when, 0);
    const QString b = DiagnosticController::uniqueReportBaseName("C:/out", when, 1);
    // Both names are byte-deterministic (dir + local-time yyyyMMdd_HHmmss_zzz + counter); the exact
    // pins prove distinctness AND the format/millisecond/counter placement the loose checks missed.
    QCOMPARE(a, QStringLiteral("C:/out/SAK_Diagnostic_20260730_120000_500_0"));
    QCOMPARE(b, QStringLiteral("C:/out/SAK_Diagnostic_20260730_120000_500_1"));
}

QTEST_MAIN(DiagnosticControllerTests)
#include "test_diagnostic_controller.moc"
