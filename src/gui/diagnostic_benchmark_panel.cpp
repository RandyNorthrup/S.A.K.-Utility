// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_benchmark_panel.cpp
/// @brief UI panel implementation for diagnostics, benchmarking, and reporting

#include "sak/diagnostic_benchmark_panel.h"

#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/generate_system_report_action.h"
#include "sak/actions/optimize_power_settings_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/detachable_log_window.h"
#include "sak/diagnostic_controller.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/quick_action_controller.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

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
    : QWidget(parent), m_controller(std::make_unique<DiagnosticController>(this)) {
    setupUi();
    connectController();
    createQuickActions();

    // Start thermal monitoring on panel creation
    m_controller->startThermalMonitoring(2000);
}

DiagnosticBenchmarkPanel::~DiagnosticBenchmarkPanel() {
    m_controller->cancelCurrent();
    m_controller->stopThermalMonitoring();
}

// ============================================================================
// UI Construction
// ============================================================================

void DiagnosticBenchmarkPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium);
    root_layout->setSpacing(sak::ui::kSpacingDefault);

    // Dynamic panel header -- updates when sub-tab changes
    m_headerWidgets = sak::createDynamicPanelHeader(
        this,
        QStringLiteral(":/icons/icons/panel_diagnostic.svg"),
        tr("Diagnostics"),
        tr("Hardware inventory, SMART analysis, and thermal monitoring"),
        root_layout);

    // Tabbed content -- Diagnostics tab and Benchmark tab
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(createDiagnosticsTab(), tr("Diagnostics"));
    m_tabs->addTab(createBenchmarkTab(), tr("Benchmarks"));
    root_layout->addWidget(m_tabs, 1);

    // Update header when sub-tab changes
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        struct TabMeta {
            const char* icon;
            const char* title;
            const char* subtitle;
        };
        static constexpr TabMeta kTabs[] = {
            {":/icons/icons/panel_diagnostic.svg",
             "Diagnostics",
             "Hardware inventory, SMART analysis, and thermal monitoring"},
            {":/icons/icons/icons8-benchmark.svg",
             "Benchmarks",
             "CPU, disk, and memory benchmarks with stress testing"},
        };
        if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
            const auto& meta = kTabs[index];
            sak::updatePanelHeader(
                m_headerWidgets, QString::fromUtf8(meta.icon), tr(meta.title), tr(meta.subtitle));
        }
    });

    // -- Log Toggle --
    m_logToggle = new sak::LogToggleSwitch(tr("Log"), this);

    auto* logToggleLayout = new QHBoxLayout();
    logToggleLayout->addWidget(m_logToggle);
    logToggleLayout->addStretch();
    root_layout->addLayout(logToggleLayout);
}

// ============================================================================
// Tab Builders
// ============================================================================

QWidget* DiagnosticBenchmarkPanel::createDiagnosticsTab() {
    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* layout = new QVBoxLayout(content_widget);
    layout->setContentsMargins(
        sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall);
    layout->setSpacing(sak::ui::kSpacingDefault);

    layout->addWidget(createHardwareSection());
    layout->addWidget(createSmartSection());
    layout->addWidget(createThermalSection());
    layout->addWidget(createSystemMaintenanceSection());
    layout->addStretch();

    scroll_area->setWidget(content_widget);
    return scroll_area;
}

QWidget* DiagnosticBenchmarkPanel::createBenchmarkTab() {
    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* layout = new QVBoxLayout(content_widget);
    layout->setContentsMargins(
        sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall);
    layout->setSpacing(sak::ui::kSpacingDefault);

    layout->addWidget(createBenchmarkSection());
    layout->addWidget(createStressTestSection());
    layout->addWidget(createSuiteSection());
    layout->addWidget(createReportSection());
    layout->addStretch();

    scroll_area->setWidget(content_widget);
    return scroll_area;
}

// ============================================================================
// Section: Hardware Inventory
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createHardwareSection() {
    auto* group = new QGroupBox("Hardware Inventory", this);
    auto* layout = new QVBoxLayout(group);

    // Key/value labels
    auto add_info_row = [&](const QString& label_text, QLabel*& value_label) {
        auto* row = new QHBoxLayout();
        auto* key_label = new QLabel(label_text, this);
        key_label->setFixedWidth(100);
        key_label->setStyleSheet("font-weight: 600;");
        row->addWidget(key_label);

        value_label = new QLabel("--", this);
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
    m_hw_rescan_button->setMinimumWidth(sak::kButtonWidthLarge);
    m_hw_rescan_button->setAccessibleName(QStringLiteral("Scan Hardware"));
    m_hw_rescan_button->setToolTip(QStringLiteral("Scan and display hardware information"));
    button_layout->addWidget(m_hw_rescan_button);

    m_hw_copy_button = new QPushButton("Copy to Clipboard", this);
    m_hw_copy_button->setMinimumWidth(sak::kButtonWidthLarge);
    m_hw_copy_button->setAccessibleName(QStringLiteral("Copy Hardware Info"));
    m_hw_copy_button->setToolTip(QStringLiteral("Copy hardware inventory to the clipboard"));
    button_layout->addWidget(m_hw_copy_button);

    layout->addLayout(button_layout);

    connect(m_hw_rescan_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onRescanHardwareClicked);
    connect(m_hw_copy_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onCopyInventoryClicked);

    Q_ASSERT(group);
    Q_ASSERT(m_hw_cpu_label);
    return group;
}

// ============================================================================
// Section: SMART Health
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createSmartSection() {
    auto* group = new QGroupBox("Storage Health (S.M.A.R.T.)", this);
    auto* layout = new QVBoxLayout(group);

    // Table
    m_smart_table = new QTableWidget(0, 6, this);
    m_smart_table->setHorizontalHeaderLabels({"Drive", "Type", "Health", "Temp", "Hours", "Wear"});
    m_smart_table->horizontalHeader()->setStretchLastSection(true);
    m_smart_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_smart_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_smart_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_smart_table->setMaximumHeight(sak::kListAreaMaxH);
    m_smart_table->verticalHeader()->setVisible(false);
    m_smart_table->setAccessibleName(QStringLiteral("SMART Health Table"));
    m_smart_table->setToolTip(QStringLiteral("S.M.A.R.T. health data for detected storage drives"));
    layout->addWidget(m_smart_table);

    // Warnings label
    m_smart_warnings_label = new QLabel("", this);
    m_smart_warnings_label->setWordWrap(true);
    m_smart_warnings_label->setStyleSheet(QString("color: %1;").arg(sak::ui::kColorWarning));
    layout->addWidget(m_smart_warnings_label);

    // Button
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();
    m_smart_rescan_button = new QPushButton("Scan SMART", this);
    m_smart_rescan_button->setMinimumWidth(sak::kButtonWidthLarge);
    m_smart_rescan_button->setAccessibleName(QStringLiteral("Scan SMART Health"));
    m_smart_rescan_button->setToolTip(
        QStringLiteral("Scan storage drives for S.M.A.R.T. health "
                       "data"));
    button_layout->addWidget(m_smart_rescan_button);
    layout->addLayout(button_layout);

    connect(m_smart_rescan_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onRescanSmartClicked);

    return group;
}

// ============================================================================
// Section: Benchmarks
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createBenchmarkSection() {
    auto* group = new QGroupBox("Benchmarks", this);
    auto* layout = new QVBoxLayout(group);

    layout->addWidget(createCpuBenchmarkGroup());
    layout->addWidget(createDiskBenchmarkGroup());
    layout->addWidget(createMemoryBenchmarkGroup());

    connect(m_cpu_benchmark_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onRunCpuBenchmarkClicked);
    connect(m_disk_benchmark_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onRunDiskBenchmarkClicked);
    connect(m_mem_benchmark_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onRunMemoryBenchmarkClicked);

    return group;
}

QGroupBox* DiagnosticBenchmarkPanel::createCpuBenchmarkGroup() {
    auto* cpu_group = new QGroupBox("CPU Benchmark", this);
    auto* cpu_layout = new QVBoxLayout(cpu_group);

    auto* cpu_scores = new QHBoxLayout();
    m_cpu_single_score_label = new QLabel("Single-Thread: \u2014", this);
    m_cpu_single_score_label->setStyleSheet("font-weight: 600;");
    cpu_scores->addWidget(m_cpu_single_score_label);

    m_cpu_multi_score_label = new QLabel("Multi-Thread: \u2014", this);
    m_cpu_multi_score_label->setStyleSheet("font-weight: 600;");
    cpu_scores->addWidget(m_cpu_multi_score_label);

    cpu_scores->addStretch();
    cpu_layout->addLayout(cpu_scores);

    m_cpu_score_bar = new QProgressBar(this);
    m_cpu_score_bar->setRange(0, 3000);
    m_cpu_score_bar->setValue(0);
    m_cpu_score_bar->setTextVisible(true);
    m_cpu_score_bar->setFormat("Score: %v / 3000 (baseline i5-12400 = 1000)");
    m_cpu_score_bar->setAccessibleName(QStringLiteral("CPU Benchmark Score"));
    m_cpu_score_bar->setAccessibleDescription(
        QStringLiteral("Displays the CPU benchmark score out "
                       "of 3000"));
    m_cpu_score_bar->setToolTip(
        QStringLiteral("CPU benchmark score relative to an i5-12400 "
                       "baseline"));
    cpu_layout->addWidget(m_cpu_score_bar);

    m_cpu_details_label = new QLabel("", this);
    m_cpu_details_label->setWordWrap(true);
    m_cpu_details_label->setStyleSheet(QString("color: %1; font-size: %2pt;")
                                           .arg(sak::ui::kColorTextSecondary)
                                           .arg(sak::ui::kFontSizeStatus));
    cpu_layout->addWidget(m_cpu_details_label);

    auto* cpu_btn_layout = new QHBoxLayout();
    cpu_btn_layout->addStretch();
    m_cpu_benchmark_button = new QPushButton("Run CPU Benchmark", this);
    m_cpu_benchmark_button->setMinimumWidth(sak::kButtonWidthXLarge);
    m_cpu_benchmark_button->setAccessibleName(QStringLiteral("Run CPU Benchmark"));
    m_cpu_benchmark_button->setToolTip(
        QStringLiteral("Run a single and multi-threaded CPU "
                       "performance test"));
    cpu_btn_layout->addWidget(m_cpu_benchmark_button);
    cpu_layout->addLayout(cpu_btn_layout);

    return cpu_group;
}

QGroupBox* DiagnosticBenchmarkPanel::createDiskBenchmarkGroup() {
    auto* disk_group = new QGroupBox("Disk I/O Benchmark", this);
    auto* disk_layout = new QVBoxLayout(disk_group);

    auto* drive_row = new QHBoxLayout();
    drive_row->addWidget(new QLabel("Drive:", this));
    m_disk_drive_combo = new QComboBox(this);
    m_disk_drive_combo->setMinimumWidth(300);
    m_disk_drive_combo->setAccessibleName(QStringLiteral("Benchmark Drive"));
    m_disk_drive_combo->setToolTip(QStringLiteral("Select a drive to benchmark"));
    drive_row->addWidget(m_disk_drive_combo);
    drive_row->addStretch();
    disk_layout->addLayout(drive_row);

    m_disk_seq_label = new QLabel("Sequential: \u2014", this);
    m_disk_seq_label->setStyleSheet("font-weight: 600;");
    disk_layout->addWidget(m_disk_seq_label);

    m_disk_rand_label = new QLabel("Random 4K: \u2014", this);
    disk_layout->addWidget(m_disk_rand_label);

    m_disk_latency_label = new QLabel("Latency: \u2014", this);
    disk_layout->addWidget(m_disk_latency_label);

    m_disk_score_label = new QLabel("Score: \u2014", this);
    m_disk_score_label->setStyleSheet(
        QString("font-weight: 600; color: %1;").arg(sak::ui::kColorPrimaryDark));
    disk_layout->addWidget(m_disk_score_label);

    m_disk_score_bar = new QProgressBar(this);
    m_disk_score_bar->setRange(0, 2000);
    m_disk_score_bar->setValue(0);
    m_disk_score_bar->setTextVisible(true);
    m_disk_score_bar->setFormat("Score: %v / 2000 (baseline 980 PRO = 1000)");
    m_disk_score_bar->setAccessibleName(QStringLiteral("Disk Benchmark Score"));
    m_disk_score_bar->setToolTip(
        QStringLiteral("Disk benchmark score relative to a Samsung 980 PRO baseline"));
    disk_layout->addWidget(m_disk_score_bar);

    auto* disk_btn_layout = new QHBoxLayout();
    disk_btn_layout->addStretch();
    m_disk_benchmark_button = new QPushButton("Run Disk Benchmark", this);
    m_disk_benchmark_button->setMinimumWidth(sak::kButtonWidthXLarge);
    m_disk_benchmark_button->setAccessibleName(QStringLiteral("Run Disk Benchmark"));
    m_disk_benchmark_button->setToolTip(
        QStringLiteral("Measure sequential and random I/O "
                       "performance"));
    disk_btn_layout->addWidget(m_disk_benchmark_button);
    disk_layout->addLayout(disk_btn_layout);

    return disk_group;
}

QGroupBox* DiagnosticBenchmarkPanel::createMemoryBenchmarkGroup() {
    auto* mem_group = new QGroupBox("Memory Benchmark", this);
    auto* mem_layout = new QVBoxLayout(mem_group);

    m_mem_bandwidth_label = new QLabel("Bandwidth: \u2014", this);
    m_mem_bandwidth_label->setStyleSheet("font-weight: 600;");
    mem_layout->addWidget(m_mem_bandwidth_label);

    m_mem_latency_label = new QLabel("Random Latency: \u2014", this);
    mem_layout->addWidget(m_mem_latency_label);

    m_mem_score_label = new QLabel("Score: \u2014", this);
    m_mem_score_label->setStyleSheet(
        QString("font-weight: 600; color: %1;").arg(sak::ui::kColorPrimaryDark));
    mem_layout->addWidget(m_mem_score_label);

    m_mem_score_bar = new QProgressBar(this);
    m_mem_score_bar->setRange(0, 2000);
    m_mem_score_bar->setValue(0);
    m_mem_score_bar->setTextVisible(true);
    m_mem_score_bar->setFormat("Score: %v / 2000 (baseline DDR4-3200 = 1000)");
    m_mem_score_bar->setAccessibleName(QStringLiteral("Memory Benchmark Score"));
    m_mem_score_bar->setToolTip(
        QStringLiteral("Memory benchmark score relative to a DDR4-3200 baseline"));
    mem_layout->addWidget(m_mem_score_bar);

    auto* mem_btn_layout = new QHBoxLayout();
    mem_btn_layout->addStretch();
    m_mem_benchmark_button = new QPushButton("Run Memory Benchmark", this);
    m_mem_benchmark_button->setMinimumWidth(sak::kButtonWidthXLarge);
    m_mem_benchmark_button->setAccessibleName(QStringLiteral("Run Memory Benchmark"));
    m_mem_benchmark_button->setToolTip(QStringLiteral("Measure memory bandwidth and latency"));
    mem_btn_layout->addWidget(m_mem_benchmark_button);
    mem_layout->addLayout(mem_btn_layout);

    return mem_group;
}


// ============================================================================
// Section: Stress Test
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createStressTestSection() {
    auto* group = new QGroupBox("Stress Testing", this);
    auto* layout = new QVBoxLayout(group);

    // Configuration row
    layout->addLayout(createStressConfigRow());

    // Status
    m_stress_status_label = new QLabel("Status: Not Running", this);
    m_stress_status_label->setStyleSheet(
        QString("font-weight: 600; color: %1;").arg(sak::ui::kColorTextHeading));
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
    layout->addLayout(createStressButtonRow());

    return group;
}

QHBoxLayout* DiagnosticBenchmarkPanel::createStressConfigRow() {
    auto* config_row = new QHBoxLayout();
    config_row->addWidget(new QLabel("Components:", this));

    m_stress_cpu_check = new QCheckBox("CPU", this);
    m_stress_cpu_check->setChecked(true);
    m_stress_cpu_check->setAccessibleName(QStringLiteral("Stress Test CPU"));
    m_stress_cpu_check->setToolTip(QStringLiteral("Include CPU in the stress test"));
    config_row->addWidget(m_stress_cpu_check);

    m_stress_memory_check = new QCheckBox("Memory", this);
    m_stress_memory_check->setChecked(true);
    m_stress_memory_check->setAccessibleName(QStringLiteral("Stress Test Memory"));
    m_stress_memory_check->setToolTip(QStringLiteral("Include memory in the stress test"));
    config_row->addWidget(m_stress_memory_check);

    m_stress_disk_check = new QCheckBox("Disk", this);
    m_stress_disk_check->setAccessibleName(QStringLiteral("Stress Test Disk"));
    m_stress_disk_check->setToolTip(QStringLiteral("Include disk I/O in the stress test"));
    config_row->addWidget(m_stress_disk_check);

    m_stress_gpu_check = new QCheckBox("GPU", this);
    m_stress_gpu_check->setAccessibleName(QStringLiteral("Stress Test GPU"));
    m_stress_gpu_check->setToolTip(QStringLiteral("Include GPU compute in the stress test"));
    config_row->addWidget(m_stress_gpu_check);

    config_row->addSpacing(20);
    config_row->addWidget(new QLabel("Duration (min):", this));
    m_stress_duration_spin = new QSpinBox(this);
    m_stress_duration_spin->setRange(1, 1440);
    m_stress_duration_spin->setValue(10);
    m_stress_duration_spin->setAccessibleName(QStringLiteral("Stress Duration"));
    m_stress_duration_spin->setToolTip(QStringLiteral("Duration of the stress test in minutes"));
    config_row->addWidget(m_stress_duration_spin);

    config_row->addSpacing(20);
    config_row->addWidget(new QLabel(QString("Thermal Limit (%1C):").arg(QChar(0x00B0)), this));
    m_stress_thermal_limit_spin = new QSpinBox(this);
    m_stress_thermal_limit_spin->setRange(60, 110);
    m_stress_thermal_limit_spin->setValue(95);
    m_stress_thermal_limit_spin->setAccessibleName(QStringLiteral("Thermal Limit"));
    m_stress_thermal_limit_spin->setToolTip(
        QStringLiteral("Maximum temperature before the stress "
                       "test is paused"));
    config_row->addWidget(m_stress_thermal_limit_spin);

    config_row->addStretch();
    return config_row;
}

QHBoxLayout* DiagnosticBenchmarkPanel::createStressButtonRow() {
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    m_stress_start_button = new QPushButton("Start Stress Test", this);
    m_stress_start_button->setMinimumWidth(sak::kButtonWidthLarge);
    m_stress_start_button->setAccessibleName(QStringLiteral("Start Stress Test"));
    m_stress_start_button->setToolTip(QStringLiteral("Begin the hardware stress test"));
    button_layout->addWidget(m_stress_start_button);

    m_stress_stop_button = new QPushButton("Stop Stress Test", this);
    m_stress_stop_button->setMinimumWidth(sak::kButtonWidthLarge);
    m_stress_stop_button->setEnabled(false);
    m_stress_stop_button->setAccessibleName(QStringLiteral("Stop Stress Test"));
    m_stress_stop_button->setToolTip(QStringLiteral("Stop the running stress test"));
    button_layout->addWidget(m_stress_stop_button);

    connect(m_stress_start_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onStartStressTestClicked);
    connect(m_stress_stop_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onStopStressTestClicked);

    return button_layout;
}

// ============================================================================
// Section: Thermal Monitor
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createThermalSection() {
    auto* group = new QGroupBox("Thermal Monitor", this);
    auto* layout = new QVBoxLayout(group);

    auto add_thermal_row =
        [&](const QString& name, QLabel*& label, QProgressBar*& bar, int max_temp) {
            auto* row = new QHBoxLayout();
            auto* name_label = new QLabel(name, this);
            name_label->setFixedWidth(100);
            name_label->setStyleSheet("font-weight: 600;");
            row->addWidget(name_label);

            label = new QLabel("0\u00B0C", this);
            label->setFixedWidth(60);
            row->addWidget(label);

            bar = new QProgressBar(this);
            bar->setRange(0, max_temp);
            bar->setValue(0);
            bar->setTextVisible(true);
            bar->setFormat("%v / %m \u00B0C");
            bar->setAccessibleName(name.chopped(1) + QStringLiteral(" Temperature"));
            bar->setAccessibleDescription(
                QStringLiteral("Current temperature reading in degrees "
                               "Celsius"));
            bar->setToolTip(QStringLiteral("Current temperature for this component"));
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

QGroupBox* DiagnosticBenchmarkPanel::createSuiteSection() {
    auto* group = new QGroupBox("Full Diagnostic Suite", this);
    auto* layout = new QVBoxLayout(group);

    // Step labels -- names stored in member array for safe reconstruction
    m_suite_step_names[0] = "Hardware Inventory";
    m_suite_step_names[1] = "SMART Disk Health";
    m_suite_step_names[2] = "CPU Benchmark";
    m_suite_step_names[3] = "Disk I/O Benchmark";
    m_suite_step_names[4] = "Memory Benchmark";
    m_suite_step_names[5] = "Stress Test";
    m_suite_step_names[6] = "Generate Report";

    for (int i = 0; i < 7; ++i) {
        m_suite_step_labels[i] =
            new QLabel(QString("  %1  %2").arg(QChar(0x23F3)).arg(m_suite_step_names[i]), this);
        m_suite_step_labels[i]->setStyleSheet(
            QString("color: %1;").arg(sak::ui::kColorTextDisabled));
        layout->addWidget(m_suite_step_labels[i]);
    }

    m_suite_status_label = new QLabel("Suite not started", this);
    m_suite_status_label->setStyleSheet(
        QString("font-weight: 600; color: %1;").arg(sak::ui::kColorTextHeading));
    layout->addWidget(m_suite_status_label);

    // Buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    m_suite_run_button = new QPushButton("Run Full Suite", this);
    m_suite_run_button->setMinimumWidth(sak::kButtonWidthLarge);
    m_suite_run_button->setAccessibleName(QStringLiteral("Run Full Suite"));
    m_suite_run_button->setToolTip(
        QStringLiteral("Run the complete diagnostic and benchmark suite"));
    m_suite_run_button->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_suite_run_button);

    m_suite_cancel_button = new QPushButton("Cancel Suite", this);
    m_suite_cancel_button->setMinimumWidth(sak::kButtonWidthMedium);
    m_suite_cancel_button->setEnabled(false);
    m_suite_cancel_button->setAccessibleName(QStringLiteral("Cancel Suite"));
    m_suite_cancel_button->setToolTip(QStringLiteral("Cancel the running diagnostic suite"));
    button_layout->addWidget(m_suite_cancel_button);

    m_suite_skip_button = new QPushButton("Skip Step", this);
    m_suite_skip_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_suite_skip_button->setEnabled(false);
    m_suite_skip_button->setAccessibleName(QStringLiteral("Skip Suite Step"));
    m_suite_skip_button->setToolTip(QStringLiteral("Skip the current step in the suite"));
    button_layout->addWidget(m_suite_skip_button);

    layout->addLayout(button_layout);

    connect(m_suite_run_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onRunFullSuiteClicked);
    connect(m_suite_cancel_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onCancelSuiteClicked);
    connect(m_suite_skip_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onSkipStepClicked);

    return group;
}

// ============================================================================
// Section: Report
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createReportSection() {
    auto* group = new QGroupBox("Report Generation", this);
    auto* layout = new QVBoxLayout(group);

    createReportInfoFields(layout);
    createReportExportButtons(layout);

    return group;
}

void DiagnosticBenchmarkPanel::createReportInfoFields(QVBoxLayout* layout) {
    // Technician / Ticket row
    auto* info_row = new QHBoxLayout();
    info_row->addWidget(new QLabel("Technician:", this));
    m_report_technician_edit = new QLineEdit(this);
    m_report_technician_edit->setPlaceholderText("Name");
    m_report_technician_edit->setAccessibleName(QStringLiteral("Technician Name"));
    m_report_technician_edit->setToolTip(
        QStringLiteral("Name of the technician generating the "
                       "report"));
    info_row->addWidget(m_report_technician_edit);

    info_row->addSpacing(20);
    info_row->addWidget(new QLabel("Ticket #:", this));
    m_report_ticket_edit = new QLineEdit(this);
    m_report_ticket_edit->setPlaceholderText("Ticket number");
    m_report_ticket_edit->setAccessibleName(QStringLiteral("Ticket Number"));
    m_report_ticket_edit->setToolTip(QStringLiteral("Ticket or work order number for the report"));
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
}

void DiagnosticBenchmarkPanel::createReportExportButtons(QVBoxLayout* layout) {
    auto* export_layout = new QHBoxLayout();
    export_layout->addStretch();

    m_report_html_button = new QPushButton("Generate HTML Report", this);
    m_report_html_button->setMinimumWidth(sak::kButtonWidthXLarge);
    m_report_html_button->setAccessibleName(QStringLiteral("Generate HTML Report"));
    m_report_html_button->setToolTip(QStringLiteral("Generate a formatted HTML diagnostic report"));
    export_layout->addWidget(m_report_html_button);

    m_report_json_button = new QPushButton("Export JSON", this);
    m_report_json_button->setMinimumWidth(sak::kButtonWidthMedium);
    m_report_json_button->setAccessibleName(QStringLiteral("Export JSON Report"));
    m_report_json_button->setToolTip(QStringLiteral("Export diagnostic data as a JSON file"));
    export_layout->addWidget(m_report_json_button);

    m_report_csv_button = new QPushButton("Export CSV", this);
    m_report_csv_button->setMinimumWidth(sak::kButtonWidthMedium);
    m_report_csv_button->setAccessibleName(QStringLiteral("Export CSV Report"));
    m_report_csv_button->setToolTip(QStringLiteral("Export diagnostic data as a CSV file"));
    export_layout->addWidget(m_report_csv_button);

    layout->addLayout(export_layout);

    connect(m_report_html_button,
            &QPushButton::clicked,
            this,
            &DiagnosticBenchmarkPanel::onGenerateReportClicked);
    connect(m_report_json_button, &QPushButton::clicked, this, [this]() {
        const auto dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
        if (!dir.isEmpty()) {
            m_controller->generateReport(dir,
                                         m_report_technician_edit->text(),
                                         m_report_ticket_edit->text(),
                                         m_report_notes_edit->toPlainText(),
                                         "json");
        }
    });
    connect(m_report_csv_button, &QPushButton::clicked, this, [this]() {
        const auto dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
        if (!dir.isEmpty()) {
            m_controller->generateReport(dir,
                                         m_report_technician_edit->text(),
                                         m_report_ticket_edit->text(),
                                         m_report_notes_edit->toPlainText(),
                                         "csv");
        }
    });
}

// ============================================================================
// Controller Signal Connections
// ============================================================================

void DiagnosticBenchmarkPanel::connectController() {
    Q_ASSERT(m_controller);
    // Hardware scan
    connect(m_controller.get(),
            &DiagnosticController::hardwareScanComplete,
            this,
            &DiagnosticBenchmarkPanel::onHardwareScanComplete);

    // SMART analysis
    connect(m_controller.get(),
            &DiagnosticController::smartAnalysisComplete,
            this,
            &DiagnosticBenchmarkPanel::onSmartAnalysisComplete);

    // Benchmarks
    connect(m_controller.get(),
            &DiagnosticController::cpuBenchmarkComplete,
            this,
            &DiagnosticBenchmarkPanel::onCpuBenchmarkComplete);
    connect(m_controller.get(),
            &DiagnosticController::diskBenchmarkComplete,
            this,
            &DiagnosticBenchmarkPanel::onDiskBenchmarkComplete);
    connect(m_controller.get(),
            &DiagnosticController::memoryBenchmarkComplete,
            this,
            &DiagnosticBenchmarkPanel::onMemoryBenchmarkComplete);

    // Stress test
    connect(m_controller.get(),
            &DiagnosticController::stressTestComplete,
            this,
            &DiagnosticBenchmarkPanel::onStressTestComplete);
    connect(m_controller.get(),
            &DiagnosticController::stressTestStatus,
            this,
            &DiagnosticBenchmarkPanel::onStressTestStatus);

    // Suite
    connect(m_controller.get(),
            &DiagnosticController::suiteStateChanged,
            this,
            &DiagnosticBenchmarkPanel::onSuiteStateChanged);
    connect(m_controller.get(),
            &DiagnosticController::suiteProgress,
            this,
            &DiagnosticBenchmarkPanel::onSuiteProgress);
    connect(m_controller.get(),
            &DiagnosticController::suiteComplete,
            this,
            &DiagnosticBenchmarkPanel::onSuiteComplete);

    // Thermal
    connect(m_controller.get(),
            &DiagnosticController::thermalReadingsUpdated,
            this,
            &DiagnosticBenchmarkPanel::onThermalReadingsUpdated);

    // Progress & errors
    connect(m_controller.get(),
            &DiagnosticController::operationProgress,
            this,
            &DiagnosticBenchmarkPanel::onOperationProgress);
    connect(m_controller.get(),
            &DiagnosticController::errorOccurred,
            this,
            &DiagnosticBenchmarkPanel::onErrorOccurred);
    connect(m_controller.get(),
            &DiagnosticController::reportsGenerated,
            this,
            &DiagnosticBenchmarkPanel::onReportsGenerated);
}

// ============================================================================
// Helpers
// ============================================================================

void DiagnosticBenchmarkPanel::setOperationRunning(bool running) {
    Q_ASSERT(m_hw_rescan_button);
    Q_ASSERT(m_smart_rescan_button);
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

void DiagnosticBenchmarkPanel::logMessage(const QString& message) {
    Q_ASSERT(!message.isEmpty());
    Q_EMIT logOutput(message);
}

QString DiagnosticBenchmarkPanel::formatBytes(uint64_t bytes) {
    return sak::formatBytes(bytes);
}

QString DiagnosticBenchmarkPanel::formatUptime(uint64_t seconds) {
    const uint64_t days = seconds / 86'400;
    const uint64_t hours = (seconds % 86'400) / 3600;
    const uint64_t minutes = (seconds % 3600) / 60;

    if (days > 0) {
        return QString("%1 day%2, %3 hour%4")
            .arg(days)
            .arg(days != 1 ? "s" : "")
            .arg(hours)
            .arg(hours != 1 ? "s" : "");
    }
    if (hours > 0) {
        return QString("%1 hour%2, %3 min").arg(hours).arg(hours != 1 ? "s" : "").arg(minutes);
    }
    return QString("%1 min").arg(minutes);
}

// ============================================================================
// Quick Actions — System Maintenance Section
// ============================================================================

QGroupBox* DiagnosticBenchmarkPanel::createSystemMaintenanceSection() {
    auto* group = new QGroupBox(tr("System Maintenance"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(sak::ui::kSpacingDefault);

    auto* desc = new QLabel(tr("One-click system verification and optimization tools"), group);
    desc->setStyleSheet(QString("color: %1; font-size: %2pt;")
                            .arg(sak::ui::kColorTextSecondary)
                            .arg(sak::ui::kFontSizeBody));
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto* btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(sak::ui::kSpacingDefault);

    auto make_button = [&](const QString& text, const QString& tooltip, const QString& key) {
        auto* btn = new QPushButton(text, group);
        btn->setMinimumHeight(sak::kButtonHeightTall);
        btn->setToolTip(tooltip);
        btn->setAccessibleName(text);
        m_qa_buttons.insert(key, btn);
        btn_layout->addWidget(btn);
    };

    make_button(tr("Optimize Power"),
                tr("Apply high-performance power settings"),
                QStringLiteral("Optimize Power Settings"));
    make_button(tr("Verify System Files"),
                tr("Run SFC scan to verify system integrity (requires admin)"),
                QStringLiteral("Verify System Files"));
    make_button(tr("Check Disk Errors"),
                tr("Scan disk for filesystem errors (requires admin)"),
                QStringLiteral("Check Disk Errors"));
    make_button(tr("Generate Report"),
                tr("Generate comprehensive system diagnostic report"),
                QStringLiteral("Generate System Report"));

    btn_layout->addStretch();
    layout->addLayout(btn_layout);

    m_qa_status_label = new QLabel(tr("Ready"), group);
    m_qa_status_label->setStyleSheet(QString("color: %1;").arg(sak::ui::kColorTextSecondary));
    layout->addWidget(m_qa_status_label);

    m_qa_progress_bar = new QProgressBar(group);
    m_qa_progress_bar->setRange(0, 100);
    m_qa_progress_bar->setValue(0);
    m_qa_progress_bar->setVisible(false);
    layout->addWidget(m_qa_progress_bar);

    return group;
}

void DiagnosticBenchmarkPanel::createQuickActions() {
    constexpr auto kDefaultBackupPath = "C:/SAK_Backups";
    const auto backup_path = QString::fromLatin1(kDefaultBackupPath);

    m_qa_controller = new QuickActionController(this);
    m_qa_controller->setBackupLocation(backup_path);

    m_qa_controller->registerAction(std::make_unique<OptimizePowerSettingsAction>());
    m_qa_controller->registerAction(std::make_unique<VerifySystemFilesAction>());
    m_qa_controller->registerAction(std::make_unique<CheckDiskErrorsAction>());
    m_qa_controller->registerAction(std::make_unique<GenerateSystemReportAction>(backup_path));

    connect(m_qa_controller,
            &QuickActionController::actionExecutionProgress,
            this,
            &DiagnosticBenchmarkPanel::onQuickActionProgress,
            Qt::QueuedConnection);
    connect(m_qa_controller,
            &QuickActionController::actionExecutionComplete,
            this,
            &DiagnosticBenchmarkPanel::onQuickActionComplete,
            Qt::QueuedConnection);
    connect(m_qa_controller,
            &QuickActionController::actionError,
            this,
            &DiagnosticBenchmarkPanel::onQuickActionError,
            Qt::QueuedConnection);
    connect(
        m_qa_controller,
        &QuickActionController::logMessage,
        this,
        [this](const QString& msg) { logMessage(msg); },
        Qt::QueuedConnection);

    for (auto it = m_qa_buttons.constBegin(); it != m_qa_buttons.constEnd(); ++it) {
        const QString action_name = it.key();
        QPushButton* btn = it.value();
        QuickAction* action = m_qa_controller->getAction(action_name);
        if (action) {
            connect(btn, &QPushButton::clicked, this, [this, action]() {
                onQuickActionClicked(action);
            });
        }
    }
}

void DiagnosticBenchmarkPanel::onQuickActionClicked(QuickAction* action) {
    Q_ASSERT(action);
    logMessage(QString("Executing: %1").arg(action->name()));
    m_qa_status_label->setText(QString("Running: %1...").arg(action->name()));
    m_qa_progress_bar->setValue(0);
    m_qa_progress_bar->setVisible(true);
    m_qa_controller->executeAction(action->name(), false);
}

void DiagnosticBenchmarkPanel::onQuickActionProgress(QuickAction* action,
                                                     const QString& message,
                                                     int progress) {
    Q_ASSERT(action);
    m_qa_status_label->setText(message);
    m_qa_progress_bar->setValue(progress);
}

void DiagnosticBenchmarkPanel::onQuickActionComplete(QuickAction* action) {
    Q_ASSERT(action);
    const auto& result = action->lastExecutionResult();
    const QString status = result.success
                               ? QString("Completed: %1").arg(action->name())
                               : QString("Failed: %1 - %2").arg(action->name(), result.message);
    m_qa_status_label->setText(status);
    m_qa_progress_bar->setVisible(false);
    logMessage(status);
    Q_EMIT statusMessage(status, sak::kTimerStatusDefaultMs);
}

void DiagnosticBenchmarkPanel::onQuickActionError(QuickAction* action,
                                                  const QString& error_message) {
    Q_ASSERT(action);
    const QString status = QString("Error: %1 - %2").arg(action->name(), error_message);
    m_qa_status_label->setText(status);
    m_qa_progress_bar->setVisible(false);
    logMessage(status);
    sak::logError("Quick action error: {} - {}",
                  action->name().toStdString(),
                  error_message.toStdString());
}

}  // namespace sak
