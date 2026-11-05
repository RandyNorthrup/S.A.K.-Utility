// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_controller.cpp
/// @brief Unit tests for DiagnosticController suite state machine, skip guard, and error handling

#include "sak/diagnostic_controller.h"
#include "sak/process_runner.h"
#include "sak/thermal_monitor.h"

#include <QMutex>
#include <QScopeGuard>
#include <QSemaphore>
#include <QtConcurrent>
#include <QThreadPool>
#include <QtTest/QtTest>

#include <optional>

using namespace sak;

class DiagnosticControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void oneScanIsNotAllTestsPassed();
    void drivesThatCouldNotBeReadAreNotAPass();
    void everyStepPassingIsAllPassed();
    void aStressRunWithNoSensorSaysSo();
    void initialState();
    void suiteStateEnum();
    void cancelCurrentResetsState();
    void reportDataAccess();
    void generateReportRefusesEachPreconditionByItsOwnMessage();
    void thermalReadingsAreForwardedToTheController();
    void aRescanAsksEveryDiskForItsTemperatureAgain();
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
    // dispatched scan hits its own cancelled checkpoint before its first query. Should the worker
    // get a query in first, the fake answers it: the real scan asks about every disk.
    const sak::ScopedProcessFaultInjector no_real_scan(
        [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            sak::ProcessResult result;
            result.exit_code = 0;
            return result;
        });
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
// The sensor script is answered by a fake: the real one asks every disk for its reliability
// counters. The reading that comes out is the fake's, which proves the forwarding carries it.
void DiagnosticControllerTests::thermalReadingsAreForwardedToTheController() {
    const sak::ScopedProcessFaultInjector fake_sensors(
        [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            sak::ProcessResult result;
            result.exit_code = 0;
            result.std_out = QStringLiteral("disk0=38");
            return result;
        });
    DiagnosticController controller;
    QSignalSpy spy(&controller, &DiagnosticController::thermalReadingsUpdated);
    QVERIFY(spy.isValid());

    controller.startThermalMonitoring(500);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 30'000);
    controller.stopThermalMonitoring();
    const auto readings = spy.first().first().value<QVector<sak::ThermalReading>>();
    QCOMPARE(readings.size(), 1);
    QCOMPARE(readings.first().component, QStringLiteral("Disk 0"));
}

// The thermal monitor stops asking a disk that gave no reading. A drive that was asleep, or was
// just plugged in, gets asked again when the technician scans. The scans themselves never start
// here: each is already "running", so the controller takes the request and ignores it, and the
// sensor script is a fake.
void DiagnosticControllerTests::aRescanAsksEveryDiskForItsTemperatureAgain() {
    const QString skips_enclosure = QStringLiteral("$r=@{};$skip=@('USB-NVME-B');");
    const QString asks_all = QStringLiteral("$r=@{};$skip=@();");
    QMutex mutex;
    QStringList scripts;
    const sak::ScopedProcessFaultInjector fake_sensors(
        [&](const QString&, const QStringList& args) -> std::optional<sak::ProcessResult> {
            const QMutexLocker lock(&mutex);
            const QString script = args.isEmpty() ? QString() : args.constLast();
            scripts.append(script);
            sak::ProcessResult result;
            result.exit_code = 0;
            result.std_out =
                QStringLiteral("identity_disk0=NVME-A\nidentity_disk1=USB-NVME-B\ndisk0=36");
            if (!script.startsWith(skips_enclosure)) {
                result.std_out += QStringLiteral("\nstatus_disk1=unavailable:no_answer");
            }
            return result;
        });
    const auto lastScriptStartsWith = [&](const QString& prefix) {
        const QMutexLocker lock(&mutex);
        return !scripts.isEmpty() && scripts.constLast().startsWith(prefix);
    };

    // The stand-in scans hold threads of their own pool until the test ends, so neither a real
    // scan nor the sensor poll's pool is involved.
    QThreadPool stand_in_pool;
    stand_in_pool.setMaxThreadCount(2);
    QSemaphore scans_may_end;
    DiagnosticController controller;
    const auto end_scans = qScopeGuard([&] {
        controller.stopThermalMonitoring();
        scans_may_end.release(2);
    });
    controller.m_hw_scan_future = QtConcurrent::run(&stand_in_pool,
                                                    [&scans_may_end] { scans_may_end.acquire(); });
    controller.m_smart_analysis_future =
        QtConcurrent::run(&stand_in_pool, [&scans_may_end] { scans_may_end.acquire(); });
    controller.startThermalMonitoring(kMinThermalPollIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(lastScriptStartsWith(skips_enclosure), 10'000);

    QVERIFY(controller.m_hw_scan_future.isRunning());
    controller.runHardwareScan();
    QTRY_VERIFY_WITH_TIMEOUT(lastScriptStartsWith(asks_all), 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(lastScriptStartsWith(skips_enclosure), 10'000);

    QVERIFY(controller.m_smart_analysis_future.isRunning());
    controller.runSmartAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(lastScriptStartsWith(asks_all), 10'000);
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

namespace {
// Every step with a good result, which is the only thing "all tests passed" may mean.
DiagnosticReportData everyStepPassed() {
    DiagnosticReportData data;
    data.inventory.cpu.name = QStringLiteral("Test CPU");
    SmartReport drive;
    drive.overall_health = SmartHealthStatus::Healthy;
    data.smart_reports = {drive};
    data.cpu_benchmark = CpuBenchmarkResult{};
    data.disk_benchmark = DiskBenchmarkResult{};
    data.memory_benchmark = MemoryBenchmarkResult{};
    StressTestResult stress;
    stress.passed = true;
    stress.max_cpu_temp = 71.0;
    data.stress_test = stress;
    return data;
}
}  // namespace

void DiagnosticControllerTests::oneScanIsNotAllTestsPassed() {
    DiagnosticController controller;
    controller.m_report_data = DiagnosticReportData{};
    controller.m_report_data.inventory.cpu.name = QStringLiteral("Test CPU");
    controller.aggregateResults();
    QCOMPARE(controller.m_report_data.overall_status, DiagnosticStatus::Warnings);
    const QString warnings = controller.m_report_data.warnings.join(QLatin1Char('\n'));
    QVERIFY2(warnings.contains(QStringLiteral("CPU benchmark has no result")),
             qPrintable(warnings));
    QVERIFY2(!warnings.contains(QStringLiteral("Hardware scan")), qPrintable(warnings));
}

void DiagnosticControllerTests::drivesThatCouldNotBeReadAreNotAPass() {
    DiagnosticController controller;
    controller.m_report_data = everyStepPassed();
    controller.m_report_data.smart_reports.first().overall_health = SmartHealthStatus::Unknown;
    controller.aggregateResults();
    QCOMPARE(controller.m_report_data.overall_status, DiagnosticStatus::Warnings);
    const QString warnings = controller.m_report_data.warnings.join(QLatin1Char('\n'));
    QVERIFY2(warnings.contains(QStringLiteral("SMART analysis has no result")),
             qPrintable(warnings));

    // One readable drive among unreadable ones is a SMART result, and the unreadable one still
    // keeps the run from reading as a clean pass.
    SmartReport readable;
    readable.overall_health = SmartHealthStatus::Healthy;
    controller.m_report_data.smart_reports.append(readable);
    QVERIFY(!DiagnosticController::stepsWithNoResult(controller.m_report_data)
                 .contains(QStringLiteral("SMART analysis")));
    controller.aggregateResults();
    QCOMPARE(controller.m_report_data.overall_status, DiagnosticStatus::Warnings);
}

void DiagnosticControllerTests::everyStepPassingIsAllPassed() {
    DiagnosticController controller;
    controller.m_report_data = everyStepPassed();
    QCOMPARE(DiagnosticController::stepsWithNoResult(controller.m_report_data), QStringList());
    controller.aggregateResults();
    QCOMPARE(controller.m_report_data.overall_status, DiagnosticStatus::AllPassed);
    QCOMPARE(controller.m_report_data.warnings, QStringList());
}

void DiagnosticControllerTests::aStressRunWithNoSensorSaysSo() {
    DiagnosticController controller;
    controller.m_report_data = everyStepPassed();
    controller.m_report_data.stress_test->max_cpu_temp = 0.0;  // Nothing was ever read.
    controller.aggregateResults();
    const QString warnings = controller.m_report_data.warnings.join(QLatin1Char('\n'));
    QVERIFY2(warnings.contains(QStringLiteral("thermal limit was not enforced")),
             qPrintable(warnings));
}

QTEST_MAIN(DiagnosticControllerTests)
#include "test_diagnostic_controller.moc"
