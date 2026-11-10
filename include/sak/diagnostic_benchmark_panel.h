// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_benchmark_panel.h
/// @brief UI panel for system diagnostics, benchmarking, stress testing, and reporting

#pragma once

#include "sak/diagnostic_controller.h"
#include "sak/diagnostic_types.h"
#include "sak/quick_action_controller.h"
#include "sak/widget_helpers.h"

#include <QTabWidget>
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

namespace ui {
class ViewEmptyState;
}  // namespace ui

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
    /// @param quick_actions Runs the System Maintenance actions; it must outlive the panel
    /// @param parent Parent widget
    explicit DiagnosticBenchmarkPanel(QuickActionController& quick_actions,
                                      QWidget* parent = nullptr);
    ~DiagnosticBenchmarkPanel() override;

    // Non-copyable, non-movable
    DiagnosticBenchmarkPanel(const DiagnosticBenchmarkPanel&) = delete;
    DiagnosticBenchmarkPanel& operator=(const DiagnosticBenchmarkPanel&) = delete;
    DiagnosticBenchmarkPanel(DiagnosticBenchmarkPanel&&) = delete;
    DiagnosticBenchmarkPanel& operator=(DiagnosticBenchmarkPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

    /// @brief How the Benchmark Drive list names a volume: its letter and label, its own size, and
    ///        the disk it is on.
    [[nodiscard]] static QString benchmarkDriveText(const StorageDeviceInfo& disk,
                                                    const PartitionInfo& volume);

    /// @brief The Battery row: the battery's name when it has one, its health, and the power
    ///        state. VirtualBox's battery names itself "1", which read as "1 - health unknown".
    [[nodiscard]] static QString batteryText(const BatteryInfo& battery);

    /// @brief What the SMART table shows for @p report, with what the hardware scan knows of the
    ///        same disk (@p disk, null when it has not run): the drive's number and model, so four
    ///        "VBOX HARDDISK" rows can be told apart and matched to the messages under them, and
    ///        its bus as the inventory names it.
    struct SmartRowIdentity {
        QString drive;
        QString type;
    };
    [[nodiscard]] static SmartRowIdentity smartRowIdentity(const SmartReport& report,
                                                           const StorageDeviceInfo* disk);

    /// @brief @p warning as the SMART list shows it: every drive's warnings are listed together,
    ///        so each starts with the drive it is about.
    [[nodiscard]] static QString smartWarningText(const QString& drive, const QString& warning);

    /// @brief "Elapsed: MM:SS" for the stress test's line.
    [[nodiscard]] static QString stressElapsedText(int elapsed_seconds);
    /// "Stress test running, 00:16 of 1 min": the status bar's message while a run goes on.
    [[nodiscard]] static QString stressRunningText(int elapsed_seconds, int duration_minutes);

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

protected:
    /// Thermal polling runs only while the readout can be seen.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private Q_SLOTS:
    // -- Hardware Inventory --------------------------------------
    void onRescanHardwareClicked();
    void onCopyInventoryClicked();
    void onHardwareScanComplete(const sak::HardwareInventory& inventory);

    // -- SMART Health --------------------------------------------
    void onRescanSmartClicked();
    void onSmartAnalysisComplete(const QVector<sak::SmartReport>& reports);

    // -- Benchmarks ----------------------------------------------
    void onRunCpuBenchmarkClicked();
    void onRunDiskBenchmarkClicked();
    void onRunMemoryBenchmarkClicked();
    void onStopBenchmarkClicked();
    void onCpuBenchmarkComplete(const sak::CpuBenchmarkResult& result);
    void onDiskBenchmarkComplete(const sak::DiskBenchmarkResult& result);
    void onMemoryBenchmarkComplete(const sak::MemoryBenchmarkResult& result);

    // -- Stress Test ---------------------------------------------
    void onStartStressTestClicked();
    void onStopStressTestClicked();
    void onStressTestComplete(const sak::StressTestResult& result);
    void onStressTestStatus(int elapsed_seconds, double cpu_temp, int errors);

    // -- Full Suite ----------------------------------------------
    void onRunFullSuiteClicked();
    void onCancelSuiteClicked();
    void onSkipStepClicked();
    void onSuiteStateChanged(sak::DiagnosticController::SuiteState state);
    void onSuiteProgress(int percent, const QString& message);
    void onSuiteComplete();

    // -- Thermal -------------------------------------------------
    void onThermalReadingsUpdated(const QVector<sak::ThermalReading>& readings);

    // -- Report --------------------------------------------------
    void onGenerateReportClicked();
    void onReportsGenerated(const QString& output_dir);

    // -- Common --------------------------------------------------
    void onOperationProgress(int percent, const QString& message);
    void onErrorOccurred(const QString& message);
    /// A drive kept with a warning: logged as one, not as an error.
    void onDriveWarning(const QString& message);

private:
    void fillSmartRow(int row, const sak::SmartReport& report, const SmartRowIdentity& identity);
    /// The hardware scan's entry for @p report's disk, or null.
    [[nodiscard]] const StorageDeviceInfo* inventoryDiskFor(const sak::SmartReport& report) const;
    static constexpr int kSuiteStepCount = 7;
    /// A suite step's marks. Pending is the idle circle; the hourglass means running elsewhere.
    static constexpr char16_t kSuitePendingMark = u'\u25CB';
    static constexpr char16_t kSuiteRunningMark = u'\u25B6';
    static constexpr char16_t kSuiteDoneMark = u'\u2705';
    static constexpr char16_t kSuiteSkippedMark = u'\u23ED';
    enum SmartTableColumn {
        SmartColDrive = 0,
        SmartColType,
        SmartColHealth,
        SmartColTemperature,
        SmartColPowerOnHours,
        SmartColWear,
        SmartColCount
    };

    /// @brief Build the complete UI layout
    void setupUi();

    /// @brief Connect all controller signals to panel slots
    void connectController();
    void connectHardwareSignals();
    void connectBenchmarkSignals();
    void connectSuiteSignals();
    void connectStatusSignals();

    /// @brief Enable/disable inputs based on running state
    void setOperationRunning(bool running);

    /// @brief Enable the operation-starting controls only when fully idle. Single
    ///        authority that mutually excludes a standalone op, the full suite, a
    ///        stress test, and a quick action so two never run on the shared
    ///        controller at once (a second would consume the first's completion).
    void refreshPrimaryControls();

    /// @brief Append a timestamped message to the log viewer
    void logMessage(const QString& message);

    /// The drive row's bar when no counter reported a drive: until a SMART scan has run the
    /// drive has not been read, which is not the same as this PC not reporting it (review 4, Y23).
    void sayWhetherTheDriveWasRead();

    /// @brief Populate disk drive combo from hardware inventory storage data
    void populateDiskDriveCombo(const HardwareInventory& inventory);

    /// @brief Add partitions of a single disk to the disk drive combo
    void addDiskPartitionsToCombo(const StorageDeviceInfo& disk);

    // -- Section Builders ----------------------------------------
    QWidget* createDiagnosticsTab();
    void createBenchmarkTab();
    QGroupBox* createHardwareSection();
    QGroupBox* createSmartSection();
    QGroupBox* createBenchmarkSection();
    QGroupBox* createCpuBenchmarkGroup();
    QGroupBox* createDiskBenchmarkGroup();
    QGroupBox* createMemoryBenchmarkGroup();
    QGroupBox* createStressTestSection();
    QGroupBox* createThermalSection();
    QGroupBox* createSuiteSection();
    void createSuiteStepLabels(QVBoxLayout* layout);
    /// Show @p step with @p mark and @p text, both in @p style.
    void showSuiteStep(int step, char16_t mark, const QString& text, const QString& style);
    QHBoxLayout* createSuiteButtonRow();
    void connectSuiteButtons();
    QGroupBox* createReportSection();

    QGroupBox* createSystemMaintenanceSection();
    void createQuickActions();
    void onQuickActionClicked(const QString& action_name);
    void onQuickActionFinished(const QuickActionRun& run,
                               const QuickAction::ExecutionResult& result,
                               QuickAction::ActionStatus status);
    /// @brief For the System Inventory Report, asks where to save it (starting in Documents)
    ///        into @p folder. Other actions write no files and pass through.
    /// @return False when the technician cancelled the folder choice, so nothing runs.
    bool chooseQuickActionOutputFolder(const QString& action_name, QString* folder);

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

    // -- Controller ----------------------------------------------
    std::unique_ptr<DiagnosticController> m_controller;

    // -- Layout --------------------------------------------------
    QTabWidget* m_tabs{nullptr};
    PanelHeaderWidgets m_headerWidgets;

    // -- Hardware Inventory Widgets ------------------------------
    QLabel* m_hw_cpu_label{nullptr};
    QLabel* m_hw_memory_label{nullptr};
    QLabel* m_hw_gpu_label{nullptr};
    QLabel* m_hw_motherboard_label{nullptr};
    QLabel* m_hw_os_label{nullptr};
    QLabel* m_hw_uptime_label{nullptr};
    QLabel* m_hw_battery_label{nullptr};
    QPushButton* m_hw_rescan_button{nullptr};
    QPushButton* m_hw_copy_button{nullptr};

    // -- SMART Health Widgets ------------------------------------
    QTableWidget* m_smart_table{nullptr};
    ui::ViewEmptyState* m_smart_empty_state{nullptr};  ///< Empty/loading overlay (R5-G20-7)
    QLabel* m_smart_warnings_label{nullptr};
    QPushButton* m_smart_rescan_button{nullptr};

    // -- CPU Benchmark Widgets -----------------------------------
    QLabel* m_cpu_single_score_label{nullptr};
    QLabel* m_cpu_multi_score_label{nullptr};
    QLabel* m_cpu_details_label{nullptr};
    QLabel* m_cpu_score_label{nullptr};
    QProgressBar* m_cpu_score_bar{nullptr};
    QPushButton* m_cpu_benchmark_button{nullptr};

    // -- Disk Benchmark Widgets ----------------------------------
    QComboBox* m_disk_drive_combo{nullptr};
    QLabel* m_disk_seq_label{nullptr};
    QLabel* m_disk_rand_label{nullptr};
    QLabel* m_disk_latency_label{nullptr};
    QLabel* m_disk_score_label{nullptr};
    QProgressBar* m_disk_score_bar{nullptr};
    QPushButton* m_disk_benchmark_button{nullptr};

    // -- Memory Benchmark Widgets --------------------------------
    QLabel* m_mem_bandwidth_label{nullptr};
    QLabel* m_mem_latency_label{nullptr};
    QLabel* m_mem_score_label{nullptr};
    QProgressBar* m_mem_score_bar{nullptr};
    QPushButton* m_mem_benchmark_button{nullptr};

    // Shared Stop for the individual CPU/disk/memory benchmarks (R5-G20-2). Enabled
    // while one of those benchmarks runs, disabled at every terminal path; clicking it
    // cooperatively stops the running benchmark via DiagnosticController::cancelCurrent.
    QPushButton* m_benchmark_stop_button{nullptr};

    // -- Stress Test Widgets -------------------------------------
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

    // -- Thermal Monitor Widgets ---------------------------------
    QLabel* m_thermal_cpu_label{nullptr};
    QLabel* m_thermal_gpu_label{nullptr};
    QLabel* m_thermal_disk_label{nullptr};
    QProgressBar* m_thermal_cpu_bar{nullptr};
    QProgressBar* m_thermal_gpu_bar{nullptr};
    QProgressBar* m_thermal_disk_bar{nullptr};
    /// The last thermal poll's unavailable-sensor lines, so N/A can say why.
    QStringList m_thermal_statuses;
    /// The hottest drive temperature the last SMART scan read, or 0: a drive Windows' counters
    /// do not report may still report it over SMART.
    double m_smart_hottest_drive_celsius{0.0};
    /// A SMART scan has run this session, so a drive with no temperature was read, not skipped.
    bool m_smart_scanned{false};

    // -- Full Suite Widgets --------------------------------------
    /// Each step's mark (pending, running, done, skipped) in a column of one width, and its name
    /// after it: in one label the marks' different widths started the names at three places
    /// (review 2, F16).
    QLabel* m_suite_step_marks[kSuiteStepCount]{nullptr};
    QLabel* m_suite_step_labels[kSuiteStepCount]{nullptr};
    QString m_suite_step_names[kSuiteStepCount];  ///< Stored for safe label reconstruction
    QLabel* m_suite_status_label{nullptr};
    QPushButton* m_suite_run_button{nullptr};
    QPushButton* m_suite_cancel_button{nullptr};
    QPushButton* m_suite_skip_button{nullptr};

    // -- Report Widgets ------------------------------------------
    QLineEdit* m_report_technician_edit{nullptr};
    QLineEdit* m_report_ticket_edit{nullptr};
    QTextEdit* m_report_notes_edit{nullptr};
    QPushButton* m_report_html_button{nullptr};
    QPushButton* m_report_json_button{nullptr};
    QPushButton* m_report_csv_button{nullptr};

    // -- Common Widgets ------------------------------------------
    LogToggleSwitch* m_logToggle{nullptr};

    // -- Quick Action Widgets ------------------------------------
    // The application's one quick-action controller, shared with the Backup and Restore and
    // Network panels and the assistant; this panel reacts only to its own runs.
    QuickActionController& m_quick_actions;
    QHash<QString, QPushButton*> m_qa_buttons;
    QProgressBar* m_qa_progress_bar{nullptr};

    // -- State ---------------------------------------------------
    bool m_operation_running{false};
    bool m_stress_test_running{false};
    bool m_suite_running{false};
    bool m_qa_running{false};
    /// Index of the suite step currently executing (-1 when none), tracked so Skip
    /// Step can record which step it skipped.
    int m_current_suite_step{-1};
    /// Per-step skip flags: a skipped step is reported distinctly instead of being
    /// shown with the same "done" checkmark as a completed one.
    bool m_suite_step_skipped[kSuiteStepCount]{};
};

}  // namespace sak
