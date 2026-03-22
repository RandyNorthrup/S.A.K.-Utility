// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_controller.cpp
/// @brief Orchestrates all diagnostic workers and the full suite sequence

#include "sak/diagnostic_controller.h"

#include "sak/cpu_benchmark_worker.h"
#include "sak/diagnostic_report_generator.h"
#include "sak/disk_benchmark_worker.h"
#include "sak/hardware_inventory_scanner.h"
#include "sak/logger.h"
#include "sak/memory_benchmark_worker.h"
#include "sak/smart_disk_analyzer.h"
#include "sak/stress_test_worker.h"
#include "sak/thermal_monitor.h"
#include "sak/worker_base.h"

#include <QDateTime>
#include <QDir>
#include <QtConcurrent>

namespace sak {

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
    // -- Hardware Scanner Connections ----------------------------
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

    // -- SMART Analyzer Connections ------------------------------
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

    // -- CPU Benchmark Connections -------------------------------
    connect(m_cpu_benchmark.get(),
            &CpuBenchmarkWorker::benchmarkComplete,
            this,
            &DiagnosticController::onCpuBenchmarkComplete);
    connect(m_cpu_benchmark.get(),
            &WorkerBase::progress,
            this,
            [this](int current, int total, const QString& msg) {
                const int percent = total > 0 ? (current * 100 / total) : 0;
                Q_EMIT operationProgress(percent, msg);
            });
    connect(
        m_cpu_benchmark.get(), &WorkerBase::failed, this, [this](int /*code*/, const QString& msg) {
            Q_EMIT errorOccurred(QString("CPU benchmark failed: %1").arg(msg));
            if (m_running_suite) {
                advanceSuiteStep();
            }
        });

    // -- Disk Benchmark Connections ------------------------------
    connect(m_disk_benchmark.get(),
            &DiskBenchmarkWorker::benchmarkComplete,
            this,
            &DiagnosticController::onDiskBenchmarkComplete);
    connect(m_disk_benchmark.get(),
            &WorkerBase::progress,
            this,
            [this](int current, int total, const QString& msg) {
                const int percent = total > 0 ? (current * 100 / total) : 0;
                Q_EMIT operationProgress(percent, msg);
            });
    connect(m_disk_benchmark.get(),
            &WorkerBase::failed,
            this,
            [this](int /*code*/, const QString& msg) {
                Q_EMIT errorOccurred(QString("Disk benchmark failed: %1").arg(msg));
                if (m_running_suite) {
                    advanceSuiteStep();
                }
            });

    // -- Memory Benchmark Connections ----------------------------
    connect(m_memory_benchmark.get(),
            &MemoryBenchmarkWorker::benchmarkComplete,
            this,
            &DiagnosticController::onMemoryBenchmarkComplete);
    connect(m_memory_benchmark.get(),
            &WorkerBase::progress,
            this,
            [this](int current, int total, const QString& msg) {
                const int percent = total > 0 ? (current * 100 / total) : 0;
                Q_EMIT operationProgress(percent, msg);
            });
    connect(m_memory_benchmark.get(),
            &WorkerBase::failed,
            this,
            [this](int /*code*/, const QString& msg) {
                Q_EMIT errorOccurred(QString("Memory benchmark failed: %1").arg(msg));
                if (m_running_suite) {
                    advanceSuiteStep();
                }
            });

    // -- Stress Test Connections ---------------------------------
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
                advanceSuiteStep();
            }
        });
    connect(m_stress_test.get(), &WorkerBase::cancelled, this, [this]() {
        // Thermal abort calls requestStop() -> emitted as cancelled().
        // Treat as completion so the suite advances.
        if (m_running_suite) {
            advanceSuiteStep();
        }
    });

    // -- Thermal Monitor Connections -----------------------------
    connect(m_thermal_monitor.get(),
            &ThermalMonitor::readingsUpdated,
            this,
            &DiagnosticController::thermalReadingsUpdated);

    // -- Report Generator Connections ----------------------------
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
    logInfo("Starting hardware inventory scan");
    // Run on a background thread to avoid blocking the UI
    m_hw_scan_future =
        QtConcurrent::run([scanner = m_hardware_scanner.get()]() { scanner->scan(); });
}

void DiagnosticController::runSmartAnalysis() {
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
    m_disk_benchmark->setConfig(config);
    if (!m_disk_benchmark->isRunning()) {
        m_disk_benchmark->start();
    }
}

void DiagnosticController::runMemoryBenchmark() {
    logInfo("Starting memory benchmark");
    if (!m_memory_benchmark->isRunning()) {
        m_memory_benchmark->start();
    }
}

void DiagnosticController::runStressTest(const StressTestConfig& config) {
    logInfo("Starting stress test ({} minutes)", config.duration_minutes);
    m_stress_test->setConfig(config);
    if (!m_stress_test->isRunning()) {
        m_stress_test->start();
    }
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
    m_suite_stress_config = stress_config;
    m_suite_disk_config = disk_config;

    // Start with hardware scan
    m_suite_state = SuiteState::HardwareScan;
    Q_EMIT suiteStateChanged(m_suite_state);
    Q_EMIT suiteProgress(0, "Scanning hardware...");
    runHardwareScan();
}

void DiagnosticController::cancelCurrent() {
    Q_ASSERT(m_hardware_scanner);
    Q_ASSERT(m_smart_analyzer);
    logInfo("Cancelling current diagnostic operation");

    m_hardware_scanner->cancel();
    m_smart_analyzer->cancel();

    // Wait for QtConcurrent futures to prevent dangling raw pointers
    if (m_hw_scan_future.isRunning()) {
        m_hw_scan_future.waitForFinished();
    }
    if (m_smart_analysis_future.isRunning()) {
        m_smart_analysis_future.waitForFinished();
    }

    if (m_cpu_benchmark->isRunning()) {
        m_cpu_benchmark->requestStop();
        m_cpu_benchmark->wait(5000);
    }
    if (m_disk_benchmark->isRunning()) {
        m_disk_benchmark->requestStop();
        m_disk_benchmark->wait(5000);
    }
    if (m_memory_benchmark->isRunning()) {
        m_memory_benchmark->requestStop();
        m_memory_benchmark->wait(5000);
    }
    if (m_stress_test->isRunning()) {
        m_stress_test->requestStop();
        m_stress_test->wait(5000);
    }

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

    // Advance to the next step after a brief delay for cleanup
    QMetaObject::invokeMethod(this, &DiagnosticController::advanceSuiteStep, Qt::QueuedConnection);
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

ThermalMonitor* DiagnosticController::thermalMonitor() const {
    return m_thermal_monitor.get();
}

// ============================================================================
// Report Generation
// ============================================================================

void DiagnosticController::generateReport(const QString& output_dir,
                                          const QString& technician,
                                          const QString& ticket,
                                          const QString& notes,
                                          const QString& formats) {
    aggregateResults();

    m_report_data.technician_name = technician;
    m_report_data.ticket_number = ticket;
    m_report_data.notes = notes;
    m_report_data.report_timestamp = QDateTime::currentDateTime();

    m_report_generator->setReportData(m_report_data);

    if (!QDir().mkpath(output_dir)) {
        sak::logWarning("Failed to create report output directory: {}", output_dir.toStdString());
    }
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString base_name = QString("%1/SAK_Diagnostic_%2").arg(output_dir, timestamp);

    bool success = true;

    if (formats.contains("html", Qt::CaseInsensitive)) {
        success &= m_report_generator->generateHtml(base_name + ".html");
    }
    if (formats.contains("json", Qt::CaseInsensitive)) {
        success &= m_report_generator->generateJson(base_name + ".json");
    }
    if (formats.contains("csv", Qt::CaseInsensitive)) {
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

    if (m_running_suite) {
        advanceSuiteStep();
    }
}

void DiagnosticController::onSmartAnalysisComplete(const QVector<SmartReport>& reports) {
    m_report_data.smart_reports = reports;
    Q_EMIT smartAnalysisComplete(reports);

    if (m_running_suite) {
        advanceSuiteStep();
    }
}

void DiagnosticController::onCpuBenchmarkComplete(const CpuBenchmarkResult& result) {
    m_report_data.cpu_benchmark = result;
    Q_EMIT cpuBenchmarkComplete(result);

    if (m_running_suite) {
        advanceSuiteStep();
    }
}

void DiagnosticController::onDiskBenchmarkComplete(const DiskBenchmarkResult& result) {
    m_report_data.disk_benchmark = result;
    Q_EMIT diskBenchmarkComplete(result);

    if (m_running_suite) {
        advanceSuiteStep();
    }
}

void DiagnosticController::onMemoryBenchmarkComplete(const MemoryBenchmarkResult& result) {
    m_report_data.memory_benchmark = result;
    Q_EMIT memoryBenchmarkComplete(result);

    if (m_running_suite) {
        advanceSuiteStep();
    }
}

void DiagnosticController::onStressTestComplete(const StressTestResult& result) {
    m_report_data.stress_test = result;
    Q_EMIT stressTestComplete(result);

    if (m_running_suite) {
        advanceSuiteStep();
    }
}

// ============================================================================
// Suite State Machine
// ============================================================================

void DiagnosticController::advanceSuiteStep() {
    m_skipping_step = false;  // Reset skip guard

    // State transition table: HW -> SMART -> CPU -> Disk -> Memory -> Stress -> Report -> Complete
    switch (m_suite_state) {
    case SuiteState::HardwareScan:
        m_suite_state = SuiteState::SmartAnalysis;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(14, "Analyzing disk health...");
        runSmartAnalysis();
        break;

    case SuiteState::SmartAnalysis:
        m_suite_state = SuiteState::CpuBenchmark;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(28, "Running CPU benchmark...");
        runCpuBenchmark();
        break;

    case SuiteState::CpuBenchmark:
        m_suite_state = SuiteState::DiskBenchmark;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(42, "Running disk benchmark...");
        runDiskBenchmark(m_suite_disk_config);
        break;

    case SuiteState::DiskBenchmark:
        m_suite_state = SuiteState::MemoryBenchmark;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(56, "Running memory benchmark...");
        runMemoryBenchmark();
        break;

    case SuiteState::MemoryBenchmark:
        if (m_suite_stress_config.duration_minutes > 0) {
            m_suite_state = SuiteState::StressTest;
            Q_EMIT suiteStateChanged(m_suite_state);
            Q_EMIT suiteProgress(70, "Running stress test...");
            runStressTest(m_suite_stress_config);
        } else {
            // Skip stress test
            m_suite_state = SuiteState::ReportGeneration;
            Q_EMIT suiteStateChanged(m_suite_state);
            Q_EMIT suiteProgress(90, "Generating report...");
            aggregateResults();
            m_suite_state = SuiteState::Complete;
            Q_EMIT suiteStateChanged(m_suite_state);
            Q_EMIT suiteProgress(100, "Suite complete");
            m_running_suite = false;
            Q_EMIT suiteComplete();
        }
        break;

    case SuiteState::StressTest:
        m_suite_state = SuiteState::ReportGeneration;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(90, "Generating report...");
        aggregateResults();
        m_suite_state = SuiteState::Complete;
        Q_EMIT suiteStateChanged(m_suite_state);
        Q_EMIT suiteProgress(100, "Suite complete");
        m_running_suite = false;
        Q_EMIT suiteComplete();
        break;

    default:
        m_running_suite = false;
        break;
    }
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
            "Run a full memory diagnostic"
            " (Windows Memory Diagnostic or MemTest86)");
    }
    if (stress.thermal_throttle_events > 0) {
        m_report_data.warnings.append(
            QString("Thermal throttling detected (%1 events)").arg(stress.thermal_throttle_events));
        m_report_data.recommendations.append(
            "Improve system cooling"
            " -- check thermal paste, fans, and airflow");
    }
}

void DiagnosticController::aggregateResults() {
    m_report_data.overall_status = DiagnosticStatus::AllPassed;
    m_report_data.critical_issues.clear();
    m_report_data.warnings.clear();
    m_report_data.recommendations.clear();

    aggregateSmartHealth();
    aggregateStressTest();

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
