// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_benchmark_panel.h
/// @brief UI panel for system diagnostics, benchmarking, stress testing, and reporting

#pragma once

#include "sak/diagnostic_controller.h"
#include "sak/diagnostic_types.h"

#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTextEdit;
class QVBoxLayout;

namespace sak {

class LogToggleSwitch;

/// @brief Diagnostic & Benchmarking panel providing hardware inventory,
///        SMART health, CPU/disk/memory benchmarks, stress testing,
///        thermal monitoring, and professional report generation.
///
/// Follows the standard SAK panel pattern with QScrollArea wrapper and
/// QGroupBox sections. All heavy operations are delegated to the
/// DiagnosticController which runs workers on background threads.
class DiagnosticBenchmarkPanel : public QWidget {
    Q_OBJECT

public:
    /// @brief Construct the diagnostic benchmark panel
    /// @param parent Parent widget
    explicit DiagnosticBenchmarkPanel(QWidget* parent = nullptr);
    ~DiagnosticBenchmarkPanel() override;

    // Non-copyable, non-movable
    DiagnosticBenchmarkPanel(const DiagnosticBenchmarkPanel&) = delete;
    DiagnosticBenchmarkPanel& operator=(const DiagnosticBenchmarkPanel&) = delete;
    DiagnosticBenchmarkPanel(DiagnosticBenchmarkPanel&&) = delete;
    DiagnosticBenchmarkPanel& operator=(DiagnosticBenchmarkPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // ── Hardware Inventory ──────────────────────────────────────
    void onRescanHardwareClicked();
    void onCopyInventoryClicked();
    void onHardwareScanComplete(const sak::HardwareInventory& inventory);

    // ── SMART Health ────────────────────────────────────────────
    void onRescanSmartClicked();
    void onSmartAnalysisComplete(const QVector<sak::SmartReport>& reports);

    // ── Benchmarks ──────────────────────────────────────────────
    void onRunCpuBenchmarkClicked();
    void onRunDiskBenchmarkClicked();
    void onRunMemoryBenchmarkClicked();
    void onCpuBenchmarkComplete(const sak::CpuBenchmarkResult& result);
    void onDiskBenchmarkComplete(const sak::DiskBenchmarkResult& result);
    void onMemoryBenchmarkComplete(const sak::MemoryBenchmarkResult& result);

    // ── Stress Test ─────────────────────────────────────────────
    void onStartStressTestClicked();
    void onStopStressTestClicked();
    void onStressTestComplete(const sak::StressTestResult& result);
    void onStressTestStatus(int elapsed_seconds, double cpu_temp, int errors);

    // ── Full Suite ──────────────────────────────────────────────
    void onRunFullSuiteClicked();
    void onCancelSuiteClicked();
    void onSkipStepClicked();
    void onSuiteStateChanged(sak::DiagnosticController::SuiteState state);
    void onSuiteProgress(int percent, const QString& message);
    void onSuiteComplete();

    // ── Thermal ─────────────────────────────────────────────────
    void onThermalReadingsUpdated(const QVector<sak::ThermalReading>& readings);

    // ── Report ──────────────────────────────────────────────────
    void onGenerateReportClicked();
    void onReportsGenerated(const QString& output_dir);

    // ── Common ──────────────────────────────────────────────────
    void onOperationProgress(int percent, const QString& message);
    void onErrorOccurred(const QString& message);

private:
    /// @brief Build the complete UI layout
    void setupUi();

    /// @brief Connect all controller signals to panel slots
    void connectController();

    /// @brief Enable/disable inputs based on running state
    void setOperationRunning(bool running);

    /// @brief Append a timestamped message to the log viewer
    void logMessage(const QString& message);

    /// @brief Populate disk drive combo from hardware inventory storage data
    void populateDiskDriveCombo(const HardwareInventory& inventory);

    /// @brief Add partitions of a single disk to the disk drive combo
    void addDiskPartitionsToCombo(const StorageDeviceInfo& disk);

    // ── Section Builders ────────────────────────────────────────
    QGroupBox* createHardwareSection();
    QGroupBox* createSmartSection();
    QGroupBox* createBenchmarkSection();
    QGroupBox* createCpuBenchmarkGroup();
    QGroupBox* createDiskBenchmarkGroup();
    QGroupBox* createMemoryBenchmarkGroup();
    QGroupBox* createStressTestSection();
    QGroupBox* createThermalSection();
    QGroupBox* createSuiteSection();
    QGroupBox* createReportSection();

    /// @brief Build technician/ticket/notes input fields for report section
    void createReportInfoFields(QVBoxLayout* layout);
    /// @brief Build export buttons (HTML/JSON/CSV) and their signal connections
    void createReportExportButtons(QVBoxLayout* layout);

    /// @brief Build the stress test configuration row (components + duration + thermal)
    QHBoxLayout* createStressConfigRow();

    /// @brief Build the stress test start/stop button row
    QHBoxLayout* createStressButtonRow();

    /// @brief Format byte count to human-readable string
    [[nodiscard]] static QString formatBytes(uint64_t bytes);

    /// @brief Format uptime to "X days, Y hours"
    [[nodiscard]] static QString formatUptime(uint64_t seconds);

    // ── Controller ──────────────────────────────────────────────
    std::unique_ptr<DiagnosticController> m_controller;

    // ── Hardware Inventory Widgets ──────────────────────────────
    QLabel* m_hw_cpu_label{nullptr};
    QLabel* m_hw_memory_label{nullptr};
    QLabel* m_hw_gpu_label{nullptr};
    QLabel* m_hw_motherboard_label{nullptr};
    QLabel* m_hw_os_label{nullptr};
    QLabel* m_hw_uptime_label{nullptr};
    QLabel* m_hw_battery_label{nullptr};
    QPushButton* m_hw_rescan_button{nullptr};
    QPushButton* m_hw_copy_button{nullptr};

    // ── SMART Health Widgets ────────────────────────────────────
    QTableWidget* m_smart_table{nullptr};
    QLabel* m_smart_warnings_label{nullptr};
    QPushButton* m_smart_rescan_button{nullptr};

    // ── CPU Benchmark Widgets ───────────────────────────────────
    QLabel* m_cpu_single_score_label{nullptr};
    QLabel* m_cpu_multi_score_label{nullptr};
    QLabel* m_cpu_details_label{nullptr};
    QProgressBar* m_cpu_score_bar{nullptr};
    QPushButton* m_cpu_benchmark_button{nullptr};

    // ── Disk Benchmark Widgets ──────────────────────────────────
    QComboBox* m_disk_drive_combo{nullptr};
    QLabel* m_disk_seq_label{nullptr};
    QLabel* m_disk_rand_label{nullptr};
    QLabel* m_disk_latency_label{nullptr};
    QLabel* m_disk_score_label{nullptr};
    QPushButton* m_disk_benchmark_button{nullptr};

    // ── Memory Benchmark Widgets ────────────────────────────────
    QLabel* m_mem_bandwidth_label{nullptr};
    QLabel* m_mem_latency_label{nullptr};
    QLabel* m_mem_score_label{nullptr};
    QPushButton* m_mem_benchmark_button{nullptr};

    // ── Stress Test Widgets ─────────────────────────────────────
    QCheckBox* m_stress_cpu_check{nullptr};
    QCheckBox* m_stress_memory_check{nullptr};
    QCheckBox* m_stress_disk_check{nullptr};
    QCheckBox* m_stress_gpu_check{nullptr};
    QSpinBox* m_stress_duration_spin{nullptr};
    QSpinBox* m_stress_thermal_limit_spin{nullptr};
    QLabel* m_stress_status_label{nullptr};
    QLabel* m_stress_elapsed_label{nullptr};
    QLabel* m_stress_temp_label{nullptr};
    QLabel* m_stress_errors_label{nullptr};
    QPushButton* m_stress_start_button{nullptr};
    QPushButton* m_stress_stop_button{nullptr};

    // ── Thermal Monitor Widgets ─────────────────────────────────
    QLabel* m_thermal_cpu_label{nullptr};
    QLabel* m_thermal_gpu_label{nullptr};
    QLabel* m_thermal_disk_label{nullptr};
    QProgressBar* m_thermal_cpu_bar{nullptr};
    QProgressBar* m_thermal_gpu_bar{nullptr};
    QProgressBar* m_thermal_disk_bar{nullptr};

    // ── Full Suite Widgets ──────────────────────────────────────
    QLabel* m_suite_step_labels[7]{nullptr};
    QString m_suite_step_names[7];            ///< Stored for safe label reconstruction
    QLabel* m_suite_status_label{nullptr};
    QPushButton* m_suite_run_button{nullptr};
    QPushButton* m_suite_cancel_button{nullptr};
    QPushButton* m_suite_skip_button{nullptr};

    // ── Report Widgets ──────────────────────────────────────────
    QLineEdit* m_report_technician_edit{nullptr};
    QLineEdit* m_report_ticket_edit{nullptr};
    QTextEdit* m_report_notes_edit{nullptr};
    QPushButton* m_report_html_button{nullptr};
    QPushButton* m_report_json_button{nullptr};
    QPushButton* m_report_csv_button{nullptr};

    // ── Common Widgets ──────────────────────────────────────────
    LogToggleSwitch* m_logToggle{nullptr};

    // ── State ───────────────────────────────────────────────────
    bool m_operation_running{false};
    bool m_stress_test_running{false};
    bool m_suite_running{false};
};

} // namespace sak
