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
    void generateReportRefusesEachPreconditionByItsOwnMessage();
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

    // The test is named for RESETTING state, and the controller is Idle on construction -- so
    // both assertions above held BEFORE cancelCurrent() was called and the emitted payload was
    // just that same untouched member. Only "one emit happened" was genuinely proved; the
    // assignment that takes a suite OUT of a mid-run state was unobserved. Drive it from a
    // non-Idle state: runFullSuite sets HardwareScan and emits before dispatching, and the
    // dispatched scan hits its own cancelled checkpoint before its first query.
    controller.runFullSuite(sak::StressTestConfig{}, sak::DiskBenchmarkConfig{});
    QVERIFY2(controller.currentState() != DiagnosticController::SuiteState::Idle,
             "control: the suite must actually leave Idle before the reset is meaningful");
    controller.cancelCurrent();
    QCOMPARE(controller.currentState(), DiagnosticController::SuiteState::Idle);
    QCOMPARE(state_spy.last()[0].value<DiagnosticController::SuiteState>(),
             DiagnosticController::SuiteState::Idle);
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
    // The other three result slots, siblings of cpu_benchmark and equally load-bearing for the
    // "empty data set" claim this test is built on.
    QVERIFY(!data.disk_benchmark.has_value());
    QVERIFY(!data.memory_benchmark.has_value());
    QVERIFY(!data.stress_test.has_value());

    // This fixture IS the hazard the m_has_results guard exists for -- a fresh controller
    // aggregates to AllPassed over nothing, which is a valid-looking passed report -- and the
    // test asserted the dangerous shape and then never the refusal that shape mandates. Nothing
    // anywhere in tests/ exercised that guard.
    QSignalSpy error_spy(&controller, &DiagnosticController::errorOccurred);
    QSignalSpy generated_spy(&controller, &DiagnosticController::reportsGenerated);
    QVERIFY(error_spy.isValid());
    QVERIFY(generated_spy.isValid());
    controller.generateReport(QDir::tempPath(),
                              QStringLiteral("tech"),
                              QStringLiteral("ticket"),
                              QStringLiteral("notes"),
                              QStringLiteral("json"));
    QCOMPARE(generated_spy.count(), 0);
    QCOMPARE(error_spy.count(), 1);
    QCOMPARE(error_spy.first()[0].toString(),
             QStringLiteral("No diagnostic results to report -- run a scan or the full suite "
                            "first"));
}

void DiagnosticControllerTests::generateReportRefusesEachPreconditionByItsOwnMessage() {
    // The format test stops at the pure parser: nothing in the tree called generateReport, so the
    // FIRST guard of the three-guard refuser was completely unobserved. Losing it re-opens
    // exactly the bug the production comment records -- with an empty `requested`, none of the
    // three writer blocks run, `success` stays true, and reportsGenerated fires having written no
    // files at all. Each guard is pinned by its OWN message, so one cannot stand in for another.
    DiagnosticController controller;
    QSignalSpy error_spy(&controller, &DiagnosticController::errorOccurred);
    QSignalSpy generated_spy(&controller, &DiagnosticController::reportsGenerated);

    // Guard 1: no known format. Checked before the directory and results guards, so it is
    // deterministic on a fresh controller.
    controller.generateReport(
        QDir::tempPath(), QString(), QString(), QString(), QStringLiteral("pdf"));
    QCOMPARE(generated_spy.count(), 0);
    QCOMPARE(error_spy.count(), 1);
    QCOMPARE(error_spy.last()[0].toString(),
             QStringLiteral("No report format requested (expected html/json/csv): 'pdf'"));

    // Guard 2: an empty output directory, which would resolve report paths to a filesystem-root
    // or relative target.
    controller.generateReport(
        QStringLiteral("   "), QString(), QString(), QString(), QStringLiteral("json"));
    QCOMPARE(generated_spy.count(), 0);
    QCOMPARE(error_spy.count(), 2);
    QCOMPARE(error_spy.last()[0].toString(), QStringLiteral("Report output directory is empty"));
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
    // ... and the OTHER direction, where the completing step is LATER than the current one. The
    // only mismatch probed had current after completedStep, so any relaxation of `==` into an
    // ordering compare in that direction survived every assertion. It is a real arrangement: a
    // worker for a step the suite has not reached yet can emit -- the stress-test cancellation
    // handler advances unconditionally while the suite is running -- and accepting it would jump
    // the suite forward from the wrong state, skipping everything in between.
    QVERIFY(!DiagnosticController::suiteAdvanceAllowed(true, S::CpuBenchmark, S::StressTest));
    QVERIFY(!DiagnosticController::suiteAdvanceAllowed(true, S::HardwareScan, S::Complete));
    // Not running -> never advance, even on a match.
    QVERIFY(!DiagnosticController::suiteAdvanceAllowed(false, S::CpuBenchmark, S::CpuBenchmark));
}

// B5-14: a suite with a failed step must not report AllPassed; a critical
// finding is never downgraded to a mere warning by the same rule.
void DiagnosticControllerTests::statusWithStepFailures_failClosed() {
    // No failures -> status unchanged. Probed with ALL THREE values, not just AllPassed: that is
    // the one input for which "return current" and "return AllPassed" are indistinguishable, so
    // two of the three no-failure cases were unclaimed. This arm matters more than the tested one
    // -- aggregateResults calls it on every aggregation with anyStepFailed == false for every
    // standalone scan and every clean suite run, exactly the runs where the SMART and stress
    // aggregators have just written CriticalIssues into overall_status.
    QCOMPARE(DiagnosticController::statusWithStepFailures(DiagnosticStatus::AllPassed, false),
             DiagnosticStatus::AllPassed);
    QCOMPARE(DiagnosticController::statusWithStepFailures(DiagnosticStatus::Warnings, false),
             DiagnosticStatus::Warnings);
    QCOMPARE(DiagnosticController::statusWithStepFailures(DiagnosticStatus::CriticalIssues, false),
             DiagnosticStatus::CriticalIssues);
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

    // The accept path's other two behaviours. No fixture had whitespace INSIDE a token or a
    // repeated token, so the per-token trim and the de-dup arm were unconstrained. The "   " case
    // below looks like it covers the trim, but it is scored by isEmpty() and stays empty either
    // way -- an untrimmed "   " is simply an unknown token, which also returns {}. Only an
    // ACCEPT-side probe reaches the trim.
    QCOMPARE(DiagnosticController::requestedReportFormats("  json  "), QStringList({"json"}));
    QCOMPARE(DiagnosticController::requestedReportFormats(" HTML , csv "),
             QStringList({"html", "csv"}));
    QCOMPARE(DiagnosticController::requestedReportFormats("json,json,JSON"), QStringList({"json"}));

    // No known format -> empty, so generateReport refuses instead of "succeeding".
    QVERIFY(DiagnosticController::requestedReportFormats("").isEmpty());
    QVERIFY(DiagnosticController::requestedReportFormats("pdf").isEmpty());
    QVERIFY(DiagnosticController::requestedReportFormats("   ").isEmpty());

    // NEAR MISSES. "pdf" shares nothing with html/json/csv, so a loosened compare stays green --
    // and the production comment names the exact prior defect a substring matcher caused:
    // "xhtml" -> html, "notjson" -> json. Re-introducing that matcher passes every other
    // assertion in this test.
    const QStringList near_misses{QStringLiteral("xhtml"),
                                  QStringLiteral("htmlx"),
                                  QStringLiteral("notjson"),
                                  QStringLiteral("jsonl"),
                                  QStringLiteral("csvx"),
                                  QStringLiteral("tsv"),
                                  QStringLiteral("ht")};
    for (const QString& token : near_misses) {
        QVERIFY2(DiagnosticController::requestedReportFormats(token).isEmpty(),
                 qPrintable(QStringLiteral("'%1' was accepted as a known format").arg(token)));
    }

    // ONE bad token voids the whole spec -- the difference between `return {}` and `continue`,
    // i.e. between refusing and silently writing a partial report. The unknown-token arm was only
    // ever reached with specs whose tokens were ALL unknown, so this fail-closed-as-a-whole
    // behaviour was unclaimed.
    QVERIFY(DiagnosticController::requestedReportFormats("html,pdf").isEmpty());
    QVERIFY(DiagnosticController::requestedReportFormats("pdf,html").isEmpty());
    QVERIFY(DiagnosticController::requestedReportFormats("html,json,xhtml,csv").isEmpty());
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
