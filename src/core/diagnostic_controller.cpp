// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_controller.cpp
/// @brief Orchestrates all diagnostic workers and the full suite sequence

#include "sak/diagnostic_controller.h"

#include "sak/cpu_benchmark_worker.h"
#include "sak/diagnostic_report_generator.h"
#include "sak/disk_benchmark_worker.h"
#include "sak/hardware_inventory_scanner.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/memory_benchmark_worker.h"
#include "sak/smart_disk_analyzer.h"
#include "sak/stress_test_worker.h"
#include "sak/thermal_monitor.h"
#include "sak/worker_base.h"

#include <QDateTime>
#include <QDir>
#include <QtConcurrent>

#include <atomic>

namespace sak {

namespace {

constexpr int kWorkerShutdownWaitMs = kTimeoutProcessShortMs;
constexpr int kSuiteSmartAnalysisProgress = 14;
constexpr int kSuiteCpuBenchmarkProgress = 28;
constexpr int kSuiteDiskBenchmarkProgress = 42;
constexpr int kSuiteMemoryBenchmarkProgress = 56;
constexpr int kSuiteStressTestProgress = 70;
constexpr int kSuiteReportGenerationProgress = 90;

/// @brief Request-stop a worker and wait for it, logging if it does not stop.
/// @return true if the worker stopped (or was not running); false on a wait
///         timeout -- a stuck worker the caller must not assume is gone (it may
///         still hold raw pointers into the controller being torn down).
bool stopWorkerAndWait(WorkerBase* worker, const char* name) {
    if (!worker->isRunning()) {
        return true;
    }
    worker->requestStop();
    if (!worker->wait(kWorkerShutdownWaitMs)) {
        logError("{} worker did not stop within {} ms during teardown -- still running",
                 name,
                 kWorkerShutdownWaitMs);
        return false;
    }
    return true;
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

DiagnosticController::DiagnosticController(QObject* parent)
    : QObject(parent)
    , m_hardware_scanner(std::make_unique<HardwareInventoryScanner>(this))
    , m_smart_analyzer(std::make_unique<SmartDiskAnalyzer>(this))
    , m_cpu_benchmark(std::make_unique<CpuBenchmarkWorker>(this))
    , m_disk_benchmark(std::make_unique<DiskBenchmarkWorker>(this))
    , m_memory_benchmark(std::make_unique<MemoryBenchmarkWorker>(this))
    , m_stress_test(std::make_unique<StressTestWorker>(this))
    , m_thermal_monitor(std::make_unique<ThermalMonitor>(this))
    , m_report_generator(std::make_unique<DiagnosticReportGenerator>(this)) {
    connectHardwareScanner();
    connectSmartAnalyzer();
    connectCpuBenchmark();
    connectDiskBenchmark();
    connectMemoryBenchmark();
    connectStressTest();
    connectThermalMonitor();
    connectReportGenerator();
}

void DiagnosticController::connectHardwareScanner() {
    connect(m_hardware_scanner.get(),
            &HardwareInventoryScanner::scanComplete,
            this,
            &DiagnosticController::onHardwareScanComplete);
    connect(m_hardware_scanner.get(),
            &HardwareInventoryScanner::scanProgress,
            this,
            [this](int percent, const QString& msg) { Q_EMIT operationProgress(percent, msg); });
    connect(m_hardware_scanner.get(),
            &HardwareInventoryScanner::errorOccurred,
            this,
            &DiagnosticController::errorOccurred);
}

void DiagnosticController::connectSmartAnalyzer() {
    connect(m_smart_analyzer.get(),
            &SmartDiskAnalyzer::analysisComplete,
            this,
            &DiagnosticController::onSmartAnalysisComplete);
    connect(m_smart_analyzer.get(),
            &SmartDiskAnalyzer::analysisProgress,
            this,
            [this](int percent, const QString& msg) { Q_EMIT operationProgress(percent, msg); });
    connect(m_smart_analyzer.get(),
            &SmartDiskAnalyzer::errorOccurred,
            this,
            &DiagnosticController::errorOccurred);
}

void DiagnosticController::connectCpuBenchmark() {
    connect(m_cpu_benchmark.get(),
            &CpuBenchmarkWorker::benchmarkComplete,
            this,
            &DiagnosticController::onCpuBenchmarkComplete);
    connect(m_cpu_benchmark.get(),
            &WorkerBase::progress,
            this,
            [this](int current, int total, const QString& msg) {
                const int percent = total > 0 ? (current * kPercentMax / total) : 0;
                Q_EMIT operationProgress(percent, msg);
            });
    connect(
        m_cpu_benchmark.get(), &WorkerBase::failed, this, [this](int /*code*/, const QString& msg) {
            Q_EMIT errorOccurred(QString("CPU benchmark failed: %1").arg(msg));
            if (m_running_suite) {
                m_suite_failures.append("CPU benchmark");
                advanceSuiteStep(SuiteState::CpuBenchmark);
            }
        });
}

void DiagnosticController::connectDiskBenchmark() {
    connect(m_disk_benchmark.get(),
            &DiskBenchmarkWorker::benchmarkComplete,
            this,
            &DiagnosticController::onDiskBenchmarkComplete);
    connect(m_disk_benchmark.get(),
            &WorkerBase::progress,
            this,
            [this](int current, int total, const QString& msg) {
                const int percent = total > 0 ? (current * kPercentMax / total) : 0;
                Q_EMIT operationProgress(percent, msg);
            });
    connect(m_disk_benchmark.get(),
            &WorkerBase::failed,
            this,
            [this](int /*code*/, const QString& msg) {
                Q_EMIT errorOccurred(QString("Disk benchmark failed: %1").arg(msg));
                if (m_running_suite) {
                    m_suite_failures.append("Disk benchmark");
                    advanceSuiteStep(SuiteState::DiskBenchmark);
                }
            });
}

void DiagnosticController::connectMemoryBenchmark() {
    connect(m_memory_benchmark.get(),
            &MemoryBenchmarkWorker::benchmarkComplete,
            this,
            &DiagnosticController::onMemoryBenchmarkComplete);
    connect(m_memory_benchmark.get(),
            &WorkerBase::progress,
            this,
            [this](int current, int total, const QString& msg) {
                const int percent = total > 0 ? (current * kPercentMax / total) : 0;
                Q_EMIT operationProgress(percent, msg);
            });
    connect(m_memory_benchmark.get(),
            &WorkerBase::failed,
            this,
            [this](int /*code*/, const QString& msg) {
                Q_EMIT errorOccurred(QString("Memory benchmark failed: %1").arg(msg));
                if (m_running_suite) {
                    m_suite_failures.append("Memory benchmark");
                    advanceSuiteStep(SuiteState::MemoryBenchmark);
                }
            });
}

void DiagnosticController::connectStressTest() {
    connect(m_stress_test.get(),
            &StressTestWorker::stressTestComplete,
            this,
            &DiagnosticController::onStressTestComplete);
    connect(m_stress_test.get(),
            &StressTestWorker::stressTestStatus,
            this,
            &DiagnosticController::stressTestStatus);
    connect(
        m_stress_test.get(), &WorkerBase::failed, this, [this](int /*code*/, const QString& msg) {
            Q_EMIT errorOccurred(QString("Stress test failed: %1").arg(msg));
            if (m_running_suite) {
                m_suite_failures.append("Stress test");
                advanceSuiteStep(SuiteState::StressTest);
            }
        });
    connect(m_stress_test.get(), &WorkerBase::cancelled, this, [this]() {
        // A user skip/cancel of the stress step emits cancelled(). Treat as
        // completion so the suite advances (state-matched, so a stale cancel
        // after the suite already moved on is ignored).
        if (m_running_suite) {
            advanceSuiteStep(SuiteState::StressTest);
        }
    });
}

void DiagnosticController::connectThermalMonitor() {
    connect(m_thermal_monitor.get(),
            &ThermalMonitor::readingsUpdated,
            this,
            &DiagnosticController::thermalReadingsUpdated);
}

void DiagnosticController::connectReportGenerator() {
    connect(m_report_generator.get(),
            &DiagnosticReportGenerator::errorOccurred,
            this,
            &DiagnosticController::errorOccurred);
}

DiagnosticController::~DiagnosticController() {
    cancelCurrent();
    m_thermal_monitor->stop();
}

// ============================================================================
// Individual Operations
// ============================================================================

void DiagnosticController::runHardwareScan() {
    // Reassigning a running QFuture orphans it (never joined), which both races scan() on the
    // shared scanner and leaves the orphan running past teardown -> UAF. Ignore redundant starts.
    if (m_hw_scan_future.isRunning()) {
        logWarning("Hardware scan already running -- ignoring request");
        return;
    }
    logInfo("Starting hardware inventory scan");
    // Run on a background thread to avoid blocking the UI
    m_hw_scan_future =
        QtConcurrent::run([scanner = m_hardware_scanner.get()]() { scanner->scan(); });
}

void DiagnosticController::runSmartAnalysis() {
    if (m_smart_analysis_future.isRunning()) {
        logWarning("SMART analysis already running -- ignoring request");
        return;
    }
    logInfo("Starting SMART disk analysis");
    // Run on a background thread to avoid blocking the UI
    m_smart_analysis_future =
        QtConcurrent::run([analyzer = m_smart_analyzer.get()]() { analyzer->analyzeAll(); });
}

void DiagnosticController::runCpuBenchmark() {
    logInfo("Starting CPU benchmark");
    if (!m_cpu_benchmark->isRunning()) {
        m_cpu_benchmark->start();
    }
}

void DiagnosticController::runDiskBenchmark(const DiskBenchmarkConfig& config) {
    logInfo("Starting disk benchmark on {}", config.drive_path.toStdString());
    // setConfig writes m_config (a QString) unsynchronized; the worker thread reads it during a
    // run, so mutate it only when the worker is stopped. Ignore a duplicate start otherwise.
    if (m_disk_benchmark->isRunning()) {
        logWarning("Disk benchmark already running -- ignoring request");
        return;
    }
    m_disk_benchmark->setConfig(config);
    m_disk_benchmark->start();
}

void DiagnosticController::runMemoryBenchmark() {
    logInfo("Starting memory benchmark");
    if (!m_memory_benchmark->isRunning()) {
        m_memory_benchmark->start();
    }
}

void DiagnosticController::runStressTest(const StressTestConfig& config) {
    logInfo("Starting stress test ({} minutes)", config.duration_minutes);
    // Same unsynchronized-setConfig race as runDiskBenchmark: only mutate config while stopped.
    if (m_stress_test->isRunning()) {
        logWarning("Stress test already running -- ignoring request");
        return;
    }
    m_stress_test->setConfig(config);
    m_stress_test->start();
}

// ============================================================================
// Full Suite
// ============================================================================

void DiagnosticController::runFullSuite(const StressTestConfig& stress_config,
                                        const DiskBenchmarkConfig& disk_config) {
    if (m_running_suite) {
        logWarning("Full suite already running -- ignoring request");
        return;
    }

    logInfo("Starting full diagnostic suite");
    m_running_suite = true;
    m_report_data = DiagnosticReportData{};
    m_suite_failures.clear();
    m_suite_stress_config = stress_config;
    m_suite_disk_config = disk_config;

    // Start with hardware scan
    m_suite_state = SuiteState::HardwareScan;
    Q_EMIT suiteStateChanged(m_suite_state);
    Q_EMIT suiteProgress(0, "Scanning hardware...");
    runHardwareScan();
}

void DiagnosticController::cancelCurrent() {
    // Invariant: both workers are make_unique'd in the constructor's member-init list and
    // never reset or released, so they are non-null for this object's whole lifetime.
    Q_ASSERT(m_hardware_scanner);
    Q_ASSERT(m_smart_analyzer);
    logInfo("Cancelling current diagnostic operation");

    m_hardware_scanner->cancel();
    m_smart_analyzer->cancel();

    // Wait for QtConcurrent futures to prevent dangling raw pointers
    if (m_hw_scan_future.isRunning()) {
        // SAK-ALLOW-BLOCKING: both scanners were cancelled on the lines above and poll that
        // flag, and their tasks hold raw pointers into this controller. Giving up on either
        // wait would leave a task dereferencing them.
        m_hw_scan_future.waitForFinished();
    }
    if (m_smart_analysis_future.isRunning()) {
        // SAK-ALLOW-BLOCKING: same contract as the hardware scan future above.
        m_smart_analysis_future.waitForFinished();
    }

    // Check each wait(): a worker that does not stop within the timeout is still
    // running, and proceeding to teardown while it holds raw pointers into this
    // controller risks a use-after-free. Surface it instead of ignoring it.
    stopWorkerAndWait(m_cpu_benchmark.get(), "CPU benchmark");
    stopWorkerAndWait(m_disk_benchmark.get(), "Disk benchmark");
    stopWorkerAndWait(m_memory_benchmark.get(), "Memory benchmark");
    stopWorkerAndWait(m_stress_test.get(), "Stress test");

    m_running_suite = false;
    m_skipping_step = false;
    m_suite_state = SuiteState::Idle;
    Q_EMIT suiteStateChanged(m_suite_state);
}

void DiagnosticController::stopStressTest() {
    logInfo("Stopping stress test");
    if (m_stress_test->isRunning()) {
        m_stress_test->requestStop();
    }
}

void DiagnosticController::skipCurrentStep() {
    // Invariant: both workers are make_unique'd in the constructor's member-init list and
    // never reset or released, so they are non-null for this object's whole lifetime.
    Q_ASSERT(m_hardware_scanner);
    Q_ASSERT(m_smart_analyzer);
    if (!m_running_suite || m_skipping_step) {
        return;
    }
    m_skipping_step = true;

    logInfo("Skipping current suite step: {}", static_cast<int>(m_suite_state));

    // Cancel the current worker
    switch (m_suite_state) {
    case SuiteState::HardwareScan:
        m_hardware_scanner->cancel();
        break;
    case SuiteState::SmartAnalysis:
        m_smart_analyzer->cancel();
        break;
    case SuiteState::CpuBenchmark:
        m_cpu_benchmark->requestStop();
        break;
    case SuiteState::DiskBenchmark:
        m_disk_benchmark->requestStop();
        break;
    case SuiteState::MemoryBenchmark:
        m_memory_benchmark->requestStop();
        break;
    case SuiteState::StressTest:
        m_stress_test->requestStop();
        break;
    default:
        break;
    }

    // Advance to the next step after a brief delay for cleanup. Capture the step
    // being skipped so the queued advance is state-matched: if the cancelled
    // worker's own completion advances first, this queued call sees a mismatch
    // and no-ops (no double-advance).
    const SuiteState skipped_step = m_suite_state;
    QMetaObject::invokeMethod(
        this, [this, skipped_step]() { advanceSuiteStep(skipped_step); }, Qt::QueuedConnection);
}

// ============================================================================
// Thermal Monitor
// ============================================================================

void DiagnosticController::startThermalMonitoring(int interval_ms) {
    m_thermal_monitor->start(interval_ms);
}

void DiagnosticController::stopThermalMonitoring() {
    m_thermal_monitor->stop();
}

// ============================================================================
// Report Generation
// ============================================================================

QStringList DiagnosticController::requestedReportFormats(const QString& formats) {
    QStringList requested;
    for (const auto& fmt :
         {QStringLiteral("html"), QStringLiteral("json"), QStringLiteral("csv")}) {
        if (formats.contains(fmt, Qt::CaseInsensitive)) {
            requested.append(fmt);
        }
    }
    return requested;
}

QString DiagnosticController::uniqueReportBaseName(const QString& output_dir,
                                                   const QDateTime& when,
                                                   quint64 counter) {
    return QString("%1/SAK_Diagnostic_%2_%3")
        .arg(output_dir, when.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")))
        .arg(counter);
}

void DiagnosticController::generateReport(const QString& output_dir,
                                          const QString& technician,
                                          const QString& ticket,
                                          const QString& notes,
                                          const QString& formats) {
    // Require at least one real output. Previously a spec naming no known format
    // wrote nothing yet still reported success.
    const QStringList requested = requestedReportFormats(formats);
    if (requested.isEmpty()) {
        logError("Report generation requested with no valid format: '{}'", formats.toStdString());
        Q_EMIT errorOccurred(
            QString("No report format requested (expected html/json/csv): '%1'").arg(formats));
        return;
    }

    aggregateResults();

    m_report_data.technician_name = technician;
    m_report_data.ticket_number = ticket;
    m_report_data.notes = notes;
    m_report_data.report_timestamp = QDateTime::currentDateTime();

    m_report_generator->setReportData(m_report_data);

    if (!QDir().mkpath(output_dir)) {
        sak::logWarning("Failed to create report output directory: {}", output_dir.toStdString());
    }
    // Millisecond timestamp + a monotonic counter so reports generated in quick
    // succession never collide on a second-resolution name.
    static std::atomic<quint64> s_report_counter{0};
    const QString base_name = uniqueReportBaseName(output_dir,
                                                   m_report_data.report_timestamp,
                                                   s_report_counter.fetch_add(1));

    bool success = true;
    if (requested.contains("html")) {
        success &= m_report_generator->generateHtml(base_name + ".html");
    }
    if (requested.contains("json")) {
        success &= m_report_generator->generateJson(base_name + ".json");
    }
    if (requested.contains("csv")) {
        success &= m_report_generator->generateCsv(base_name + ".csv");
    }

    if (success) {
        logInfo("Reports generated in {}", output_dir.toStdString());
        Q_EMIT reportsGenerated(output_dir);
    } else {
        logError("Report generation failed for {}", output_dir.toStdString());
        Q_EMIT errorOccurred(QString("Report generation failed in %1").arg(output_dir));
    }
}

// ============================================================================
// Completion Handlers
// ============================================================================

void DiagnosticController::onHardwareScanComplete(const HardwareInventory& inventory) {
    m_report_data.inventory = inventory;
    Q_EMIT hardwareScanComplete(inventory);
    advanceSuiteStep(SuiteState::HardwareScan);
}

void DiagnosticController::onSmartAnalysisComplete(const QVector<SmartReport>& reports) {
    m_report_data.smart_reports = reports;
    Q_EMIT smartAnalysisComplete(reports);
    advanceSuiteStep(SuiteState::SmartAnalysis);
}

void DiagnosticController::onCpuBenchmarkComplete(const CpuBenchmarkResult& result) {
    m_report_data.cpu_benchmark = result;
    Q_EMIT cpuBenchmarkComplete(result);
    advanceSuiteStep(SuiteState::CpuBenchmark);
}

void DiagnosticController::onDiskBenchmarkComplete(const DiskBenchmarkResult& result) {
    m_report_data.disk_benchmark = result;
    Q_EMIT diskBenchmarkComplete(result);
    advanceSuiteStep(SuiteState::DiskBenchmark);
}

void DiagnosticController::onMemoryBenchmarkComplete(const MemoryBenchmarkResult& result) {
    m_report_data.memory_benchmark = result;
    Q_EMIT memoryBenchmarkComplete(result);
    advanceSuiteStep(SuiteState::MemoryBenchmark);
}

void DiagnosticController::onStressTestComplete(const StressTestResult& result) {
    m_report_data.stress_test = result;
    Q_EMIT stressTestComplete(result);
    advanceSuiteStep(SuiteState::StressTest);
}

// ============================================================================
// Suite State Machine
// ============================================================================

bool DiagnosticController::suiteAdvanceAllowed(bool runningSuite,
                                               SuiteState current,
                                               SuiteState completedStep) {
    // Only the worker for the step currently in progress may advance the suite.
    // A stale completion (a cancelled/skipped worker whose signal arrives after
    // the suite already moved on, or skipCurrentStep's own queued advance) will
    // not match and is ignored -- preventing a double-advance that skips a step.
    return runningSuite && current == completedStep;
}

void DiagnosticController::advanceSuiteStep(SuiteState completedStep) {
    if (!suiteAdvanceAllowed(m_running_suite, m_suite_state, completedStep)) {
        return;
    }
    m_skipping_step = false;  // Reset skip guard (this advance is the accepted one)

    // State transition table: HW -> SMART -> CPU -> Disk -> Memory -> Stress -> Report -> Complete
    switch (m_suite_state) {
    case SuiteState::HardwareScan:
        m_suite_state = SuiteState::SmartAnalysis;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(kSuiteSmartAnalysisProgress, "Analyzing disk health...");
        runSmartAnalysis();
        break;

    case SuiteState::SmartAnalysis:
        m_suite_state = SuiteState::CpuBenchmark;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(kSuiteCpuBenchmarkProgress, "Running CPU benchmark...");
        runCpuBenchmark();
        break;

    case SuiteState::CpuBenchmark:
        m_suite_state = SuiteState::DiskBenchmark;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(kSuiteDiskBenchmarkProgress, "Running disk benchmark...");
        runDiskBenchmark(m_suite_disk_config);
        break;

    case SuiteState::DiskBenchmark:
        m_suite_state = SuiteState::MemoryBenchmark;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(kSuiteMemoryBenchmarkProgress, "Running memory benchmark...");
        runMemoryBenchmark();
        break;

    case SuiteState::MemoryBenchmark:
        if (m_suite_stress_config.duration_minutes > 0) {
            m_suite_state = SuiteState::StressTest;
            Q_EMIT suiteStateChanged(m_suite_state);
            Q_EMIT suiteProgress(kSuiteStressTestProgress, "Running stress test...");
            runStressTest(m_suite_stress_config);
        } else {
            finalizeSuiteAndComplete();  // stress test skipped (duration 0)
        }
        break;

    case SuiteState::StressTest:
        finalizeSuiteAndComplete();
        break;

    default:
        m_running_suite = false;
        break;
    }
}

void DiagnosticController::finalizeSuiteAndComplete() {
    m_suite_state = SuiteState::ReportGeneration;
    Q_EMIT suiteStateChanged(m_suite_state);
    Q_EMIT suiteProgress(kSuiteReportGenerationProgress, "Generating report...");
    aggregateResults();
    m_suite_state = SuiteState::Complete;
    Q_EMIT suiteStateChanged(m_suite_state);
    Q_EMIT suiteProgress(kPercentMax, "Suite complete");
    m_running_suite = false;
    Q_EMIT suiteComplete();
}

// ============================================================================
// Result Aggregation
// ============================================================================

void DiagnosticController::aggregateSmartHealth() {
    for (const auto& report : m_report_data.smart_reports) {
        if (report.overall_health == SmartHealthStatus::Critical) {
            m_report_data.overall_status = DiagnosticStatus::CriticalIssues;
            m_report_data.critical_issues.append(QString("Drive %1 (%2): CRITICAL health status")
                                                     .arg(report.device_path, report.model));
        }

        const bool is_warning = (report.overall_health == SmartHealthStatus::Warning);
        if (is_warning && m_report_data.overall_status == DiagnosticStatus::AllPassed) {
            m_report_data.overall_status = DiagnosticStatus::Warnings;
        }
        if (is_warning) {
            m_report_data.warnings.append(QString("Drive %1 (%2): Warning health status")
                                              .arg(report.device_path, report.model));
        }
        m_report_data.warnings.append(report.warnings);
        m_report_data.recommendations.append(report.recommendations);
    }
}

void DiagnosticController::aggregateStressTest() {
    if (!m_report_data.stress_test.has_value()) {
        return;
    }
    const auto& stress = m_report_data.stress_test.value();
    if (!stress.passed) {
        m_report_data.overall_status = DiagnosticStatus::CriticalIssues;
        m_report_data.critical_issues.append(
            QString("Stress test FAILED: %1").arg(stress.abort_reason));
    }
    if (stress.memory_pattern_errors > 0) {
        m_report_data.critical_issues.append(QString("Memory pattern errors detected: %1"
                                                     " -- possible RAM hardware issue")
                                                 .arg(stress.memory_pattern_errors));
        m_report_data.recommendations.append(
            "Run a full memory diagnostic with Windows Memory Diagnostic");
    }
    if (stress.thermal_throttle_events > 0) {
        m_report_data.warnings.append(
            QString("Thermal throttling detected (%1 events)").arg(stress.thermal_throttle_events));
        m_report_data.recommendations.append(
            "Improve system cooling"
            " -- check thermal paste, fans, and airflow");
    }
}

DiagnosticStatus DiagnosticController::statusWithStepFailures(DiagnosticStatus current,
                                                              bool anyStepFailed) {
    if (!anyStepFailed) {
        return current;
    }
    // A failed step means a result is missing -- the run cannot be "all passed".
    // Never upgrade: a critical finding elsewhere outranks a missing benchmark.
    return current == DiagnosticStatus::AllPassed ? DiagnosticStatus::Warnings : current;
}

void DiagnosticController::aggregateResults() {
    m_report_data.overall_status = DiagnosticStatus::AllPassed;
    m_report_data.critical_issues.clear();
    m_report_data.warnings.clear();
    m_report_data.recommendations.clear();

    aggregateSmartHealth();
    aggregateStressTest();

    // A suite step that failed produced no result -- do not let the aggregate
    // read AllPassed as though every component succeeded.
    for (const QString& step : m_suite_failures) {
        m_report_data.warnings.append(
            QString("%1 did not complete successfully -- results unavailable").arg(step));
    }
    m_report_data.overall_status = statusWithStepFailures(m_report_data.overall_status,
                                                          !m_suite_failures.isEmpty());

    constexpr double kBatteryWarningThreshold = 50.0;
    if (m_report_data.inventory.battery.present &&
        m_report_data.inventory.battery.health_percent < kBatteryWarningThreshold) {
        m_report_data.warnings.append(
            QString("Battery health is degraded: %1%")
                .arg(m_report_data.inventory.battery.health_percent, 0, 'f', 1));
        m_report_data.recommendations.append("Consider replacing the battery");
    }

    logInfo(
        "Aggregation complete -- status: {}, {} critical,"
        " {} warnings",
        static_cast<int>(m_report_data.overall_status),
        m_report_data.critical_issues.size(),
        m_report_data.warnings.size());
}

}  // namespace sak
