// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_controller.h
/// @brief Orchestrates all diagnostic workers and coordinates the full suite

#pragma once

#include "sak/diagnostic_types.h"

#include <QFuture>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

namespace sak {

// Forward declarations
class HardwareInventoryScanner;
class SmartDiskAnalyzer;
class CpuBenchmarkWorker;
class DiskBenchmarkWorker;
class MemoryBenchmarkWorker;
class StressTestWorker;
class ThermalMonitor;
class DiagnosticReportGenerator;

/// @brief Central orchestrator for all diagnostic and benchmarking operations
///
/// Coordinates the lifecycle of all diagnostic workers, manages the full
/// diagnostic suite sequence, and aggregates results for report generation.
/// Acts as the single point of contact between the UI panel and the
/// backend workers.
///
/// Owns all worker instances and manages their lifetime via std::unique_ptr.
///
/// Usage:
/// @code
///   auto controller = std::make_unique<DiagnosticController>(this);
///   connect(controller.get(), &DiagnosticController::hardwareScanComplete, ...);
///   controller->runHardwareScan();
///   // or
///   controller->runFullSuite(config);
/// @endcode
class DiagnosticController : public QObject {
    Q_OBJECT

public:
    /// @brief Current state of the full diagnostic suite
    enum class SuiteState {
        Idle,
        HardwareScan,
        SmartAnalysis,
        CpuBenchmark,
        DiskBenchmark,
        MemoryBenchmark,
        StressTest,
        ReportGeneration,
        Complete
    };
    Q_ENUM(SuiteState)

    /// @brief Construct a DiagnosticController
    /// @param parent Parent QObject
    explicit DiagnosticController(QObject* parent = nullptr);
    ~DiagnosticController() override;

    // Non-copyable, non-movable
    DiagnosticController(const DiagnosticController&) = delete;
    DiagnosticController& operator=(const DiagnosticController&) = delete;
    DiagnosticController(DiagnosticController&&) = delete;
    DiagnosticController& operator=(DiagnosticController&&) = delete;

    // ── Individual Operations ───────────────────────────────────

    /// @brief Run hardware inventory scan
    void runHardwareScan();

    /// @brief Run SMART analysis on all drives
    void runSmartAnalysis();

    /// @brief Run CPU benchmark
    void runCpuBenchmark();

    /// @brief Run disk benchmark on a specific drive
    /// @param config Benchmark configuration
    void runDiskBenchmark(const DiskBenchmarkConfig& config);

    /// @brief Run memory benchmark
    void runMemoryBenchmark();

    /// @brief Run stress test
    /// @param config Stress test configuration
    void runStressTest(const StressTestConfig& config);

    // ── Full Suite ──────────────────────────────────────────────

    /// @brief Run the complete diagnostic suite in sequence
    /// @param stress_config Stress test configuration (optional; skipped if duration is 0)
    /// @param disk_config Disk benchmark configuration
    void runFullSuite(const StressTestConfig& stress_config,
                      const DiskBenchmarkConfig& disk_config);

    /// @brief Cancel the currently running operation or suite
    void cancelCurrent();

    /// @brief Stop only the stress test worker (does not affect other operations)
    void stopStressTest();

    /// @brief Skip the current step in the full suite (advance to next)
    void skipCurrentStep();

    // ── Thermal Monitor ─────────────────────────────────────────

    /// @brief Start thermal monitoring
    /// @param interval_ms Poll interval in milliseconds
    void startThermalMonitoring(int interval_ms = 2000);

    /// @brief Stop thermal monitoring
    void stopThermalMonitoring();

    // ── Report Generation ───────────────────────────────────────

    /// @brief Generate report from all collected data
    /// @param output_dir Directory to save reports
    /// @param technician Technician name
    /// @param ticket Ticket number
    /// @param notes Additional notes
    /// @param formats Comma-separated: "html,json,csv"
    void generateReport(const QString& output_dir,
                        const QString& technician,
                        const QString& ticket,
                        const QString& notes,
                        const QString& formats = "html,json,csv");

    // ── Accessors ───────────────────────────────────────────────

    /// @brief Get the current suite state
    [[nodiscard]] SuiteState currentState() const { return m_suite_state; }

    /// @brief Get the aggregated report data
    [[nodiscard]] const DiagnosticReportData& reportData() const { return m_report_data; }

    /// @brief Get the thermal monitor instance for direct connection
    [[nodiscard]] ThermalMonitor* thermalMonitor() const;

Q_SIGNALS:
    // ── Scan / Analysis Results ─────────────────────────────────

    /// @brief Emitted when hardware scan completes
    void hardwareScanComplete(const sak::HardwareInventory& inventory);

    /// @brief Emitted when SMART analysis completes
    void smartAnalysisComplete(const QVector<sak::SmartReport>& reports);

    /// @brief Emitted when CPU benchmark completes
    void cpuBenchmarkComplete(const sak::CpuBenchmarkResult& result);

    /// @brief Emitted when disk benchmark completes
    void diskBenchmarkComplete(const sak::DiskBenchmarkResult& result);

    /// @brief Emitted when memory benchmark completes
    void memoryBenchmarkComplete(const sak::MemoryBenchmarkResult& result);

    /// @brief Emitted when stress test completes
    void stressTestComplete(const sak::StressTestResult& result);

    // ── Suite Signals ───────────────────────────────────────────

    /// @brief Emitted when suite state changes
    void suiteStateChanged(sak::DiagnosticController::SuiteState state);

    /// @brief Emitted with overall progress of the full suite
    /// @param percent Progress percentage (0-100)
    /// @param message Current step description
    void suiteProgress(int percent, const QString& message);

    /// @brief Emitted when the full suite completes
    void suiteComplete();

    // ── Progress & Status ───────────────────────────────────────

    /// @brief Progress for the current individual operation
    void operationProgress(int percent, const QString& message);

    /// @brief Error from any worker
    void errorOccurred(const QString& message);

    /// @brief Reports generated successfully
    void reportsGenerated(const QString& output_dir);

    /// @brief Thermal readings for UI update
    void thermalReadingsUpdated(const QVector<sak::ThermalReading>& readings);

    /// @brief Stress test live status
    void stressTestStatus(int elapsed_seconds, double cpu_temp, int errors);

private Q_SLOTS:
    /// @brief Handle hardware scan completion
    void onHardwareScanComplete(const sak::HardwareInventory& inventory);

    /// @brief Handle SMART analysis completion
    void onSmartAnalysisComplete(const QVector<sak::SmartReport>& reports);

    /// @brief Handle CPU benchmark completion
    void onCpuBenchmarkComplete(const sak::CpuBenchmarkResult& result);

    /// @brief Handle disk benchmark completion
    void onDiskBenchmarkComplete(const sak::DiskBenchmarkResult& result);

    /// @brief Handle memory benchmark completion
    void onMemoryBenchmarkComplete(const sak::MemoryBenchmarkResult& result);

    /// @brief Handle stress test completion
    void onStressTestComplete(const sak::StressTestResult& result);

private:
    /// @brief Advance to the next step in the full suite
    void advanceSuiteStep();

    /// @brief Aggregate all data and determine overall status
    void aggregateResults();

    // Workers (created on-demand)
    std::unique_ptr<HardwareInventoryScanner> m_hardware_scanner;
    std::unique_ptr<SmartDiskAnalyzer> m_smart_analyzer;
    std::unique_ptr<CpuBenchmarkWorker> m_cpu_benchmark;
    std::unique_ptr<DiskBenchmarkWorker> m_disk_benchmark;
    std::unique_ptr<MemoryBenchmarkWorker> m_memory_benchmark;
    std::unique_ptr<StressTestWorker> m_stress_test;
    std::unique_ptr<ThermalMonitor> m_thermal_monitor;
    std::unique_ptr<DiagnosticReportGenerator> m_report_generator;

    // State
    SuiteState m_suite_state{SuiteState::Idle};
    std::atomic<bool> m_running_suite{false};
    std::atomic<bool> m_skipping_step{false};  ///< Guards skipCurrentStep from double-advance
    DiagnosticReportData m_report_data;

    // Async operation futures (prevent fire-and-forget)
    QFuture<void> m_hw_scan_future;
    QFuture<void> m_smart_analysis_future;

    // Suite configuration (saved when runFullSuite is called)
    StressTestConfig m_suite_stress_config;
    DiskBenchmarkConfig m_suite_disk_config;
};

}  // namespace sak
