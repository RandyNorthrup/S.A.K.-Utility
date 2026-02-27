// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_benchmark_panel.cpp
/// @brief UI panel implementation for diagnostics, benchmarking, and reporting

#include "sak/diagnostic_benchmark_panel.h"
#include "sak/diagnostic_controller.h"
#include "sak/logger.h"
#include "sak/detachable_log_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <iterator>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

DiagnosticBenchmarkPanel::DiagnosticBenchmarkPanel(QWidget* parent)
    : QWidget(parent)
    , m_controller(std::make_unique<DiagnosticController>(this))
{
    setupUi();
    connectController();

    // Start thermal monitoring on panel creation
    m_controller->startThermalMonitoring(2000);
}

DiagnosticBenchmarkPanel::~DiagnosticBenchmarkPanel()
{
    m_controller->cancelCurrent();
    m_controller->stopThermalMonitoring();
}

// ============================================================================
// UI Construction
// ============================================================================

void DiagnosticBenchmarkPanel::setupUi()
{
    // Root: zero margin → scroll area
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setContentsMargins(12, 12, 12, 12);
    main_layout->setSpacing(10);

    scroll_area->setWidget(content_widget);
    root_layout->addWidget(scroll_area);

    // â”€â”€ Sections â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    main_layout->addWidget(createHardwareSection());
    main_layout->addWidget(createSmartSection());
    main_layout->addWidget(createBenchmarkSection());
    main_layout->addWidget(createStressTestSection());
    main_layout->addWidget(createThermalSection());
    main_layout->addWidget(createSuiteSection());
    main_layout->addWidget(createReportSection());

    // -- Progress Section --


    // -- Log Toggle --
    m_logToggle = new sak::LogToggleSwitch(tr("Log"), this);

    auto* logToggleLayout = new QHBoxLayout();
    logToggleLayout->addWidget(m_logToggle);
    logToggleLayout->addStretch();
    main_layout->addLayout(logToggleLayout);

    // Final stretch
    main_layout->addStretch();
}

// ============================================================================
// Section: Hardware Inventory
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createHardwareSection()
{
    auto* group = new QGroupBox("Hardware Inventory", this);
    auto* layout = new QVBoxLayout(group);

    // Key/value labels
    auto add_info_row = [&](const QString& label_text, QLabel*& value_label) {
        auto* row = new QHBoxLayout();
        auto* key_label = new QLabel(label_text, this);
        key_label->setFixedWidth(100);
        key_label->setStyleSheet("font-weight: 600;");
        row->addWidget(key_label);

        value_label = new QLabel("—", this);
        value_label->setWordWrap(true);
        row->addWidget(value_label, 1);
        layout->addLayout(row);
    };

    add_info_row("CPU:", m_hw_cpu_label);
    add_info_row("Memory:", m_hw_memory_label);
    add_info_row("GPU:", m_hw_gpu_label);
    add_info_row("Motherboard:", m_hw_motherboard_label);
    add_info_row("OS:", m_hw_os_label);
    add_info_row("Uptime:", m_hw_uptime_label);
    add_info_row("Battery:", m_hw_battery_label);

    // Buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    m_hw_rescan_button = new QPushButton("Scan Hardware", this);
    m_hw_rescan_button->setMinimumWidth(140);
    button_layout->addWidget(m_hw_rescan_button);

    m_hw_copy_button = new QPushButton("Copy to Clipboard", this);
    m_hw_copy_button->setMinimumWidth(140);
    button_layout->addWidget(m_hw_copy_button);

    layout->addLayout(button_layout);

    connect(m_hw_rescan_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onRescanHardwareClicked);
    connect(m_hw_copy_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onCopyInventoryClicked);

    return group;
}

// ============================================================================
// Section: SMART Health
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createSmartSection()
{
    auto* group = new QGroupBox("Storage Health (S.M.A.R.T.)", this);
    auto* layout = new QVBoxLayout(group);

    // Table
    m_smart_table = new QTableWidget(0, 6, this);
    m_smart_table->setHorizontalHeaderLabels(
        {"Drive", "Type", "Health", "Temp", "Hours", "Wear"});
    m_smart_table->horizontalHeader()->setStretchLastSection(true);
    m_smart_table->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    m_smart_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_smart_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_smart_table->setMaximumHeight(160);
    m_smart_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_smart_table);

    // Warnings label
    m_smart_warnings_label = new QLabel("", this);
    m_smart_warnings_label->setWordWrap(true);
    m_smart_warnings_label->setStyleSheet("color: #ea580c;");
    layout->addWidget(m_smart_warnings_label);

    // Button
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();
    m_smart_rescan_button = new QPushButton("Scan SMART", this);
    m_smart_rescan_button->setMinimumWidth(140);
    button_layout->addWidget(m_smart_rescan_button);
    layout->addLayout(button_layout);

    connect(m_smart_rescan_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onRescanSmartClicked);

    return group;
}

// ============================================================================
// Section: Benchmarks
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createBenchmarkSection()
{
    auto* group = new QGroupBox("Benchmarks", this);
    auto* layout = new QVBoxLayout(group);

    // â”€â”€ CPU Benchmark â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* cpu_group = new QGroupBox("CPU Benchmark", this);
    auto* cpu_layout = new QVBoxLayout(cpu_group);

    auto* cpu_scores = new QHBoxLayout();
    m_cpu_single_score_label = new QLabel("Single-Thread: —", this);
    m_cpu_single_score_label->setStyleSheet("font-weight: 600;");
    cpu_scores->addWidget(m_cpu_single_score_label);

    m_cpu_multi_score_label = new QLabel("Multi-Thread: —", this);
    m_cpu_multi_score_label->setStyleSheet("font-weight: 600;");
    cpu_scores->addWidget(m_cpu_multi_score_label);

    cpu_scores->addStretch();
    cpu_layout->addLayout(cpu_scores);

    m_cpu_score_bar = new QProgressBar(this);
    m_cpu_score_bar->setRange(0, 3000);
    m_cpu_score_bar->setValue(0);
    m_cpu_score_bar->setTextVisible(true);
    m_cpu_score_bar->setFormat("Score: %v / 3000 (baseline i5-12400 = 1000)");
    cpu_layout->addWidget(m_cpu_score_bar);

    m_cpu_details_label = new QLabel("", this);
    m_cpu_details_label->setWordWrap(true);
    m_cpu_details_label->setStyleSheet("color: #475569; font-size: 11px;");
    cpu_layout->addWidget(m_cpu_details_label);

    auto* cpu_btn_layout = new QHBoxLayout();
    cpu_btn_layout->addStretch();
    m_cpu_benchmark_button = new QPushButton("Run CPU Benchmark", this);
    m_cpu_benchmark_button->setMinimumWidth(160);
    cpu_btn_layout->addWidget(m_cpu_benchmark_button);
    cpu_layout->addLayout(cpu_btn_layout);

    layout->addWidget(cpu_group);

    // â”€â”€ Disk Benchmark â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* disk_group = new QGroupBox("Disk I/O Benchmark", this);
    auto* disk_layout = new QVBoxLayout(disk_group);

    auto* drive_row = new QHBoxLayout();
    drive_row->addWidget(new QLabel("Drive:", this));
    m_disk_drive_combo = new QComboBox(this);
    m_disk_drive_combo->setMinimumWidth(300);
    drive_row->addWidget(m_disk_drive_combo);
    drive_row->addStretch();
    disk_layout->addLayout(drive_row);

    m_disk_seq_label = new QLabel("Sequential: —", this);
    m_disk_seq_label->setStyleSheet("font-weight: 600;");
    disk_layout->addWidget(m_disk_seq_label);

    m_disk_rand_label = new QLabel("Random 4K: —", this);
    disk_layout->addWidget(m_disk_rand_label);

    m_disk_latency_label = new QLabel("Latency: —", this);
    disk_layout->addWidget(m_disk_latency_label);

    m_disk_score_label = new QLabel("Score: —", this);
    m_disk_score_label->setStyleSheet("font-weight: 600; color: #2563eb;");
    disk_layout->addWidget(m_disk_score_label);

    auto* disk_btn_layout = new QHBoxLayout();
    disk_btn_layout->addStretch();
    m_disk_benchmark_button = new QPushButton("Run Disk Benchmark", this);
    m_disk_benchmark_button->setMinimumWidth(160);
    disk_btn_layout->addWidget(m_disk_benchmark_button);
    disk_layout->addLayout(disk_btn_layout);

    layout->addWidget(disk_group);

    // â”€â”€ Memory Benchmark â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* mem_group = new QGroupBox("Memory Benchmark", this);
    auto* mem_layout = new QVBoxLayout(mem_group);

    m_mem_bandwidth_label = new QLabel("Bandwidth: —", this);
    m_mem_bandwidth_label->setStyleSheet("font-weight: 600;");
    mem_layout->addWidget(m_mem_bandwidth_label);

    m_mem_latency_label = new QLabel("Random Latency: —", this);
    mem_layout->addWidget(m_mem_latency_label);

    m_mem_score_label = new QLabel("Score: —", this);
    m_mem_score_label->setStyleSheet("font-weight: 600; color: #2563eb;");
    mem_layout->addWidget(m_mem_score_label);

    auto* mem_btn_layout = new QHBoxLayout();
    mem_btn_layout->addStretch();
    m_mem_benchmark_button = new QPushButton("Run Memory Benchmark", this);
    m_mem_benchmark_button->setMinimumWidth(160);
    mem_btn_layout->addWidget(m_mem_benchmark_button);
    mem_layout->addLayout(mem_btn_layout);

    layout->addWidget(mem_group);

    // Connect buttons
    connect(m_cpu_benchmark_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onRunCpuBenchmarkClicked);
    connect(m_disk_benchmark_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onRunDiskBenchmarkClicked);
    connect(m_mem_benchmark_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onRunMemoryBenchmarkClicked);

    return group;
}

// ============================================================================
// Section: Stress Test
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createStressTestSection()
{
    auto* group = new QGroupBox("Stress Testing", this);
    auto* layout = new QVBoxLayout(group);

    // Configuration row
    auto* config_row = new QHBoxLayout();
    config_row->addWidget(new QLabel("Components:", this));

    m_stress_cpu_check = new QCheckBox("CPU", this);
    m_stress_cpu_check->setChecked(true);
    config_row->addWidget(m_stress_cpu_check);

    m_stress_memory_check = new QCheckBox("Memory", this);
    m_stress_memory_check->setChecked(true);
    config_row->addWidget(m_stress_memory_check);

    m_stress_disk_check = new QCheckBox("Disk", this);
    config_row->addWidget(m_stress_disk_check);

    config_row->addSpacing(20);
    config_row->addWidget(new QLabel("Duration (min):", this));
    m_stress_duration_spin = new QSpinBox(this);
    m_stress_duration_spin->setRange(1, 1440);
    m_stress_duration_spin->setValue(10);
    config_row->addWidget(m_stress_duration_spin);

    config_row->addSpacing(20);
    config_row->addWidget(new QLabel("Thermal Limit (°C):", this));
    m_stress_thermal_limit_spin = new QSpinBox(this);
    m_stress_thermal_limit_spin->setRange(60, 110);
    m_stress_thermal_limit_spin->setValue(95);
    config_row->addWidget(m_stress_thermal_limit_spin);

    config_row->addStretch();
    layout->addLayout(config_row);

    // Status
    m_stress_status_label = new QLabel("Status: Not Running", this);
    m_stress_status_label->setStyleSheet("font-weight: 600; color: #1e293b;");
    layout->addWidget(m_stress_status_label);

    // Live stats row
    auto* stats_row = new QHBoxLayout();
    m_stress_elapsed_label = new QLabel("", this);
    stats_row->addWidget(m_stress_elapsed_label);

    m_stress_temp_label = new QLabel("", this);
    stats_row->addWidget(m_stress_temp_label);

    m_stress_errors_label = new QLabel("", this);
    stats_row->addWidget(m_stress_errors_label);

    stats_row->addStretch();
    layout->addLayout(stats_row);

    // Buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    m_stress_start_button = new QPushButton("Start Stress Test", this);
    m_stress_start_button->setMinimumWidth(140);
    button_layout->addWidget(m_stress_start_button);

    m_stress_stop_button = new QPushButton("Stop Stress Test", this);
    m_stress_stop_button->setMinimumWidth(140);
    m_stress_stop_button->setEnabled(false);
    button_layout->addWidget(m_stress_stop_button);

    layout->addLayout(button_layout);

    connect(m_stress_start_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onStartStressTestClicked);
    connect(m_stress_stop_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onStopStressTestClicked);

    return group;
}

// ============================================================================
// Section: Thermal Monitor
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createThermalSection()
{
    auto* group = new QGroupBox("Thermal Monitor", this);
    auto* layout = new QVBoxLayout(group);

    auto add_thermal_row = [&](const QString& name, QLabel*& label,
                               QProgressBar*& bar, int max_temp) {
        auto* row = new QHBoxLayout();
        auto* name_label = new QLabel(name, this);
        name_label->setFixedWidth(100);
        name_label->setStyleSheet("font-weight: 600;");
        row->addWidget(name_label);

        label = new QLabel("—°C", this);
        label->setFixedWidth(60);
        row->addWidget(label);

        bar = new QProgressBar(this);
        bar->setRange(0, max_temp);
        bar->setValue(0);
        bar->setTextVisible(true);
        bar->setFormat("%v / %m °C");
        row->addWidget(bar, 1);

        layout->addLayout(row);
    };

    add_thermal_row("CPU:", m_thermal_cpu_label, m_thermal_cpu_bar, 100);
    add_thermal_row("GPU:", m_thermal_gpu_label, m_thermal_gpu_bar, 100);
    add_thermal_row("Disk 0:", m_thermal_disk_label, m_thermal_disk_bar, 70);

    return group;
}

// ============================================================================
// Section: Full Suite
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createSuiteSection()
{
    auto* group = new QGroupBox("Full Diagnostic Suite", this);
    auto* layout = new QVBoxLayout(group);

    // Step labels — names stored in member array for safe reconstruction
    m_suite_step_names[0] = "Hardware Inventory";
    m_suite_step_names[1] = "SMART Disk Health";
    m_suite_step_names[2] = "CPU Benchmark";
    m_suite_step_names[3] = "Disk I/O Benchmark";
    m_suite_step_names[4] = "Memory Benchmark";
    m_suite_step_names[5] = "Stress Test";
    m_suite_step_names[6] = "Generate Report";

    for (int i = 0; i < 7; ++i) {
        m_suite_step_labels[i] = new QLabel(
            QString("  %1  %2").arg(QChar(0x23F3)).arg(m_suite_step_names[i]), this);
        m_suite_step_labels[i]->setStyleSheet("color: #94a3b8;");
        layout->addWidget(m_suite_step_labels[i]);
    }

    m_suite_status_label = new QLabel("Suite not started", this);
    m_suite_status_label->setStyleSheet("font-weight: 600; color: #1e293b;");
    layout->addWidget(m_suite_status_label);

    // Buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    m_suite_run_button = new QPushButton("Run Full Suite", this);
    m_suite_run_button->setMinimumWidth(140);
    button_layout->addWidget(m_suite_run_button);

    m_suite_cancel_button = new QPushButton("Cancel Suite", this);
    m_suite_cancel_button->setMinimumWidth(120);
    m_suite_cancel_button->setEnabled(false);
    button_layout->addWidget(m_suite_cancel_button);

    m_suite_skip_button = new QPushButton("Skip Step", this);
    m_suite_skip_button->setMinimumWidth(100);
    m_suite_skip_button->setEnabled(false);
    button_layout->addWidget(m_suite_skip_button);

    layout->addLayout(button_layout);

    connect(m_suite_run_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onRunFullSuiteClicked);
    connect(m_suite_cancel_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onCancelSuiteClicked);
    connect(m_suite_skip_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onSkipStepClicked);

    return group;
}

// ============================================================================
// Section: Report
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createReportSection()
{
    auto* group = new QGroupBox("Report Generation", this);
    auto* layout = new QVBoxLayout(group);

    // Technician / Ticket row
    auto* info_row = new QHBoxLayout();
    info_row->addWidget(new QLabel("Technician:", this));
    m_report_technician_edit = new QLineEdit(this);
    m_report_technician_edit->setPlaceholderText("Name");
    info_row->addWidget(m_report_technician_edit);

    info_row->addSpacing(20);
    info_row->addWidget(new QLabel("Ticket #:", this));
    m_report_ticket_edit = new QLineEdit(this);
    m_report_ticket_edit->setPlaceholderText("Ticket number");
    info_row->addWidget(m_report_ticket_edit);

    layout->addLayout(info_row);

    // Notes
    auto* notes_row = new QHBoxLayout();
    notes_row->addWidget(new QLabel("Notes:", this));
    m_report_notes_edit = new QTextEdit(this);
    m_report_notes_edit->setMaximumHeight(60);
    m_report_notes_edit->setPlaceholderText("Additional notes for the report...");
    notes_row->addWidget(m_report_notes_edit);
    layout->addLayout(notes_row);

    // Export buttons
    auto* export_layout = new QHBoxLayout();
    export_layout->addStretch();

    m_report_html_button = new QPushButton("Generate HTML Report", this);
    m_report_html_button->setMinimumWidth(160);
    export_layout->addWidget(m_report_html_button);

    m_report_json_button = new QPushButton("Export JSON", this);
    m_report_json_button->setMinimumWidth(120);
    export_layout->addWidget(m_report_json_button);

    m_report_csv_button = new QPushButton("Export CSV", this);
    m_report_csv_button->setMinimumWidth(120);
    export_layout->addWidget(m_report_csv_button);

    layout->addLayout(export_layout);

    connect(m_report_html_button, &QPushButton::clicked,
            this, &DiagnosticBenchmarkPanel::onGenerateReportClicked);
    connect(m_report_json_button, &QPushButton::clicked, this, [this]() {
        const auto dir = QFileDialog::getExistingDirectory(
            this, "Select Output Directory");
        if (!dir.isEmpty()) {
            m_controller->generateReport(
                dir,
                m_report_technician_edit->text(),
                m_report_ticket_edit->text(),
                m_report_notes_edit->toPlainText(),
                "json");
        }
    });
    connect(m_report_csv_button, &QPushButton::clicked, this, [this]() {
        const auto dir = QFileDialog::getExistingDirectory(
            this, "Select Output Directory");
        if (!dir.isEmpty()) {
            m_controller->generateReport(
                dir,
                m_report_technician_edit->text(),
                m_report_ticket_edit->text(),
                m_report_notes_edit->toPlainText(),
                "csv");
        }
    });

    return group;
}

// ============================================================================
// Controller Signal Connections
// ============================================================================

void DiagnosticBenchmarkPanel::connectController()
{
    // Hardware scan
    connect(m_controller.get(), &DiagnosticController::hardwareScanComplete,
            this, &DiagnosticBenchmarkPanel::onHardwareScanComplete);

    // SMART analysis
    connect(m_controller.get(), &DiagnosticController::smartAnalysisComplete,
            this, &DiagnosticBenchmarkPanel::onSmartAnalysisComplete);

    // Benchmarks
    connect(m_controller.get(), &DiagnosticController::cpuBenchmarkComplete,
            this, &DiagnosticBenchmarkPanel::onCpuBenchmarkComplete);
    connect(m_controller.get(), &DiagnosticController::diskBenchmarkComplete,
            this, &DiagnosticBenchmarkPanel::onDiskBenchmarkComplete);
    connect(m_controller.get(), &DiagnosticController::memoryBenchmarkComplete,
            this, &DiagnosticBenchmarkPanel::onMemoryBenchmarkComplete);

    // Stress test
    connect(m_controller.get(), &DiagnosticController::stressTestComplete,
            this, &DiagnosticBenchmarkPanel::onStressTestComplete);
    connect(m_controller.get(), &DiagnosticController::stressTestStatus,
            this, &DiagnosticBenchmarkPanel::onStressTestStatus);

    // Suite
    connect(m_controller.get(), &DiagnosticController::suiteStateChanged,
            this, &DiagnosticBenchmarkPanel::onSuiteStateChanged);
    connect(m_controller.get(), &DiagnosticController::suiteProgress,
            this, &DiagnosticBenchmarkPanel::onSuiteProgress);
    connect(m_controller.get(), &DiagnosticController::suiteComplete,
            this, &DiagnosticBenchmarkPanel::onSuiteComplete);

    // Thermal
    connect(m_controller.get(), &DiagnosticController::thermalReadingsUpdated,
            this, &DiagnosticBenchmarkPanel::onThermalReadingsUpdated);

    // Progress & errors
    connect(m_controller.get(), &DiagnosticController::operationProgress,
            this, &DiagnosticBenchmarkPanel::onOperationProgress);
    connect(m_controller.get(), &DiagnosticController::errorOccurred,
            this, &DiagnosticBenchmarkPanel::onErrorOccurred);
    connect(m_controller.get(), &DiagnosticController::reportsGenerated,
            this, &DiagnosticBenchmarkPanel::onReportsGenerated);
}

// ============================================================================
// Helpers
// ============================================================================

void DiagnosticBenchmarkPanel::setOperationRunning(bool running)
{
    m_operation_running = running;

    m_hw_rescan_button->setEnabled(!running);
    m_smart_rescan_button->setEnabled(!running);
    m_cpu_benchmark_button->setEnabled(!running);
    m_disk_benchmark_button->setEnabled(!running);
    m_mem_benchmark_button->setEnabled(!running);

    if (!m_stress_test_running) {
        m_stress_start_button->setEnabled(!running);
    }

    if (running) {
        Q_EMIT statusMessage("Running...", 0);
        Q_EMIT progressUpdate(0, 100);
    } else {
        Q_EMIT statusMessage("Ready", 3000);
        Q_EMIT progressUpdate(100, 100);
    }
}

void DiagnosticBenchmarkPanel::logMessage(const QString& message)
{
    Q_EMIT logOutput(message);
}

QString DiagnosticBenchmarkPanel::formatBytes(uint64_t bytes)
{
    if (bytes == 0) return "0 B";

    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    auto value = static_cast<double>(bytes);

    while (value >= 1024.0 && unit_index < 4) {
        value /= 1024.0;
        ++unit_index;
    }

    return QString("%1 %2").arg(value, 0, 'f', unit_index > 0 ? 1 : 0)
                           .arg(units[unit_index]);
}

QString DiagnosticBenchmarkPanel::formatUptime(uint64_t seconds)
{
    const uint64_t days = seconds / 86400;
    const uint64_t hours = (seconds % 86400) / 3600;
    const uint64_t minutes = (seconds % 3600) / 60;

    if (days > 0) {
        return QString("%1 day%2, %3 hour%4")
            .arg(days).arg(days != 1 ? "s" : "")
            .arg(hours).arg(hours != 1 ? "s" : "");
    }
    if (hours > 0) {
        return QString("%1 hour%2, %3 min")
            .arg(hours).arg(hours != 1 ? "s" : "")
            .arg(minutes);
    }
    return QString("%1 min").arg(minutes);
}

} // namespace sak
