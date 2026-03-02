// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_benchmark_panel_slots.cpp
/// @brief Slot handler implementations for DiagnosticBenchmarkPanel

#include "sak/diagnostic_benchmark_panel.h"
#include "sak/diagnostic_controller.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/layout_constants.h"

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
// Slot: Hardware Inventory
// ============================================================================

void DiagnosticBenchmarkPanel::onRescanHardwareClicked()
{
    logMessage("Starting hardware inventory scan...");
    setOperationRunning(true);
    m_controller->runHardwareScan();
}

void DiagnosticBenchmarkPanel::onCopyInventoryClicked()
{
    const auto& report_data = m_controller->reportData();
    const auto& inv = report_data.inventory;

    QString text;
    text += QString("CPU: %1 (%2C/%3T) @ %4 MHz\n")
                .arg(inv.cpu.name)
                .arg(inv.cpu.cores)
                .arg(inv.cpu.threads)
                .arg(inv.cpu.max_clock_mhz);
    text += QString("Memory: %1 (%2/%3 slots)\n")
                .arg(formatBytes(inv.memory.total_bytes))
                .arg(inv.memory.slots_used)
                .arg(inv.memory.slots_total);
    for (const auto& gpu : inv.gpus) {
        text += QString("GPU: %1 (%2)\n").arg(gpu.name, formatBytes(gpu.vram_bytes));
    }
    text += QString("Motherboard: %1 %2\n")
                .arg(inv.motherboard.manufacturer, inv.motherboard.product);
    text += QString("OS: %1 %2 (Build %3)\n")
                .arg(inv.os_name, inv.os_version, inv.os_build);
    text += QString("Uptime: %1\n").arg(formatUptime(inv.uptime_seconds));

    QApplication::clipboard()->setText(text);
    logMessage("Hardware inventory copied to clipboard");
    Q_EMIT statusMessage("Hardware inventory copied to clipboard", sak::kTimerStatusMessageMs);
}

void DiagnosticBenchmarkPanel::onHardwareScanComplete(
    const HardwareInventory& inventory)
{
    setOperationRunning(false);

    m_hw_cpu_label->setText(
        QString("%1 (%2C/%3T) @ %4 MHz")
            .arg(inventory.cpu.name)
            .arg(inventory.cpu.cores)
            .arg(inventory.cpu.threads)
            .arg(inventory.cpu.max_clock_mhz));

    m_hw_memory_label->setText(
        QString("%1 (%2/%3 slots)")
            .arg(formatBytes(inventory.memory.total_bytes))
            .arg(inventory.memory.slots_used)
            .arg(inventory.memory.slots_total));

    if (!inventory.gpus.isEmpty()) {
        QStringList gpu_lines;
        for (const auto& gpu : inventory.gpus) {
            gpu_lines.append(QString("%1 (%2)")
                                 .arg(gpu.name, formatBytes(gpu.vram_bytes)));
        }
        m_hw_gpu_label->setText(gpu_lines.join("; "));
    }

    m_hw_motherboard_label->setText(
        QString("%1 %2 (BIOS: %3)")
            .arg(inventory.motherboard.manufacturer,
                 inventory.motherboard.product,
                 inventory.motherboard.bios_version));

    m_hw_os_label->setText(
        QString("%1 %2 (Build %3)")
            .arg(inventory.os_name, inventory.os_version, inventory.os_build));

    m_hw_uptime_label->setText(formatUptime(inventory.uptime_seconds));

    if (inventory.battery.present) {
        m_hw_battery_label->setText(
            QString("%1 — %2% health, %3")
                .arg(inventory.battery.name)
                .arg(inventory.battery.health_percent, 0, 'f', 1)
                .arg(inventory.battery.status));
    } else {
        m_hw_battery_label->setText("Not present (desktop)");
    }

    // Populate disk drive combo for disk benchmark
    populateDiskDriveCombo(inventory);

    logMessage("Hardware inventory scan complete");
    Q_EMIT statusMessage("Hardware scan complete", sak::kTimerStatusMessageMs);

    // Automatically trigger SMART analysis after hardware scan
    logMessage("Starting SMART disk analysis...");
    setOperationRunning(true);
    m_controller->runSmartAnalysis();
}

void DiagnosticBenchmarkPanel::populateDiskDriveCombo(
    const HardwareInventory& inventory)
{
    m_disk_drive_combo->clear();
    for (const auto& disk : inventory.storage) {
        addDiskPartitionsToCombo(disk);
    }
}

void DiagnosticBenchmarkPanel::addDiskPartitionsToCombo(
    const StorageDeviceInfo& disk)
{
    for (const auto& part : disk.partitions) {
        if (part.drive_letter.isEmpty()) continue;
        m_disk_drive_combo->addItem(
            QString("%1 (%2 - %3)")
                .arg(part.drive_letter, disk.model,
                     formatBytes(disk.size_bytes)),
            part.drive_letter + "\\");
    }
}

// ============================================================================
// Slot: SMART Health
// ============================================================================

void DiagnosticBenchmarkPanel::onRescanSmartClicked()
{
    logMessage("Starting SMART disk analysis...");
    setOperationRunning(true);
    m_controller->runSmartAnalysis();
}

void DiagnosticBenchmarkPanel::onSmartAnalysisComplete(
    const QVector<SmartReport>& reports)
{
    setOperationRunning(false);

    m_smart_table->setRowCount(reports.size());
    QStringList warnings_list;

    for (int row = 0; row < reports.size(); ++row) {
        const auto& report = reports[row];

        m_smart_table->setItem(row, 0, new QTableWidgetItem(report.model));
        m_smart_table->setItem(row, 1, new QTableWidgetItem(report.interface_type));

        // Health with icon
        auto* health_item = new QTableWidgetItem();
        switch (report.overall_health) {
            case SmartHealthStatus::Healthy:
                health_item->setText("PASS");
                health_item->setForeground(QColor(sak::ui::kStatusColorSuccess));
                break;
            case SmartHealthStatus::Warning:
                health_item->setText("WARN");
                health_item->setForeground(QColor(sak::ui::kStatusColorWarning));
                break;
            case SmartHealthStatus::Critical:
                health_item->setText("FAIL");
                health_item->setForeground(QColor(sak::ui::kStatusColorError));
                break;
            default:
                health_item->setText("N/A");
                break;
        }
        m_smart_table->setItem(row, 2, health_item);

        m_smart_table->setItem(row, 3,
            new QTableWidgetItem(QString("%1°C").arg(report.temperature_celsius, 0, 'f', 0)));

        m_smart_table->setItem(row, 4,
            new QTableWidgetItem(
                QString::number(report.power_on_hours)));

        // Wear level: NVMe percentage or "—" for SATA
        const QString wear = report.nvme_health.has_value()
            ? QString("%1%").arg(report.nvme_health->percentage_used)
            : "—";
        m_smart_table->setItem(row, 5, new QTableWidgetItem(wear));

        // Collect warnings
        for (const auto& warning : report.warnings) {
            warnings_list.append(warning);
        }
    }

    m_smart_warnings_label->setText(warnings_list.join("\n"));
    m_smart_warnings_label->setVisible(!warnings_list.isEmpty());

    logMessage(QString("SMART analysis complete: %1 drive(s) analyzed")
                   .arg(reports.size()));
    Q_EMIT statusMessage("SMART analysis complete", sak::kTimerStatusMessageMs);
}

// ============================================================================
// Slot: CPU Benchmark
// ============================================================================

void DiagnosticBenchmarkPanel::onRunCpuBenchmarkClicked()
{
    logMessage("Starting CPU benchmark...");
    setOperationRunning(true);
    m_controller->runCpuBenchmark();
}

void DiagnosticBenchmarkPanel::onCpuBenchmarkComplete(
    const CpuBenchmarkResult& result)
{
    setOperationRunning(false);

    m_cpu_single_score_label->setText(
        QString("Single-Thread: %1").arg(result.single_thread_score));
    m_cpu_multi_score_label->setText(
        QString("Multi-Thread: %1").arg(result.multi_thread_score));

    m_cpu_score_bar->setValue(result.multi_thread_score);

    const double baseline_pct =
        result.multi_thread_score > 0
            ? (result.multi_thread_score / 10.0)
            : 0.0;

    m_cpu_details_label->setText(
        QString("Prime Sieve: %1 ms | Matrix: %2 ms (%3 GFLOPS) | "
                "ZLIB: %4 MB/s | AES: %5 MB/s | Scaling: %6% | "
                "%7% of baseline (i5-12400)")
            .arg(result.prime_sieve_time_ms, 0, 'f', 1)
            .arg(result.matrix_multiply_time_ms, 0, 'f', 1)
            .arg(result.matrix_gflops, 0, 'f', 2)
            .arg(result.zlib_throughput_mbps, 0, 'f', 1)
            .arg(result.aes_throughput_mbps, 0, 'f', 1)
            .arg(result.thread_scaling_efficiency * 100.0, 0, 'f', 1)
            .arg(baseline_pct, 0, 'f', 0));

    logMessage(QString("CPU benchmark complete — Score: %1 (ST) / %2 (MT)")
                   .arg(result.single_thread_score)
                   .arg(result.multi_thread_score));
    Q_EMIT statusMessage("CPU benchmark complete", sak::kTimerStatusMessageMs);
}

// ============================================================================
// Slot: Disk Benchmark
// ============================================================================

void DiagnosticBenchmarkPanel::onRunDiskBenchmarkClicked()
{
    if (m_disk_drive_combo->count() == 0) {
        logMessage("No drives available — run Hardware Scan first");
        Q_EMIT statusMessage("No drives — run Hardware Scan first", sak::kTimerStatusMessageMs);
        return;
    }

    DiskBenchmarkConfig config;
    config.drive_path = m_disk_drive_combo->currentData().toString();
    config.test_file_size_mb = 1024;

    logMessage(QString("Starting disk benchmark on %1...")
                   .arg(config.drive_path));
    setOperationRunning(true);
    m_controller->runDiskBenchmark(config);
}

void DiagnosticBenchmarkPanel::onDiskBenchmarkComplete(
    const DiskBenchmarkResult& result)
{
    setOperationRunning(false);

    m_disk_seq_label->setText(
        QString("Sequential — Read: %1 MB/s | Write: %2 MB/s")
            .arg(result.seq_read_mbps, 0, 'f', 1)
            .arg(result.seq_write_mbps, 0, 'f', 1));

    m_disk_rand_label->setText(
        QString("Random 4K QD1 — R: %1 MB/s (%2 IOPS) | W: %3 MB/s (%4 IOPS)\n"
                "Random 4K QD32 — R: %5 MB/s (%6 IOPS) | W: %7 MB/s (%8 IOPS)")
            .arg(result.rand_4k_read_mbps, 0, 'f', 1)
            .arg(result.rand_4k_read_iops, 0, 'f', 0)
            .arg(result.rand_4k_write_mbps, 0, 'f', 1)
            .arg(result.rand_4k_write_iops, 0, 'f', 0)
            .arg(result.rand_4k_qd32_read_mbps, 0, 'f', 1)
            .arg(result.rand_4k_qd32_read_iops, 0, 'f', 0)
            .arg(result.rand_4k_qd32_write_mbps, 0, 'f', 1)
            .arg(result.rand_4k_qd32_write_iops, 0, 'f', 0));

    m_disk_latency_label->setText(
        QString("Latency — Read avg: %1 \u03BCs (P99: %2 \u03BCs) | "
                "Write avg: %3 \u03BCs (P99: %4 \u03BCs)")
            .arg(result.avg_read_latency_us, 0, 'f', 1)
            .arg(result.p99_read_latency_us, 0, 'f', 1)
            .arg(result.avg_write_latency_us, 0, 'f', 1)
            .arg(result.p99_write_latency_us, 0, 'f', 1));

    m_disk_score_label->setText(
        QString("Score: %1 (Samsung 980 PRO = 1000)")
            .arg(result.overall_score));

    logMessage(QString("Disk benchmark complete — Score: %1")
                   .arg(result.overall_score));
    Q_EMIT statusMessage("Disk benchmark complete", sak::kTimerStatusMessageMs);
}

// ============================================================================
// Slot: Memory Benchmark
// ============================================================================

void DiagnosticBenchmarkPanel::onRunMemoryBenchmarkClicked()
{
    logMessage("Starting memory benchmark...");
    setOperationRunning(true);
    m_controller->runMemoryBenchmark();
}

void DiagnosticBenchmarkPanel::onMemoryBenchmarkComplete(
    const MemoryBenchmarkResult& result)
{
    setOperationRunning(false);

    m_mem_bandwidth_label->setText(
        QString("Read: %1 GB/s | Write: %2 GB/s | Copy: %3 GB/s")
            .arg(result.read_bandwidth_gbps, 0, 'f', 1)
            .arg(result.write_bandwidth_gbps, 0, 'f', 1)
            .arg(result.copy_bandwidth_gbps, 0, 'f', 1));

    m_mem_latency_label->setText(
        QString("Random Latency: %1 ns | Alloc Stress: %2 ops/s | "
                "Max Contiguous: %3 MB")
            .arg(result.random_latency_ns, 0, 'f', 1)
            .arg(result.alloc_dealloc_ops_per_sec, 0, 'f', 0)
            .arg(result.max_contiguous_alloc_mb));

    m_mem_score_label->setText(
        QString("Score: %1 (DDR4-3200 dual-channel = 1000)")
            .arg(result.overall_score));

    logMessage(QString("Memory benchmark complete — Score: %1")
                   .arg(result.overall_score));
    Q_EMIT statusMessage("Memory benchmark complete", sak::kTimerStatusMessageMs);
}

// ============================================================================
// Slot: Stress Test
// ============================================================================

void DiagnosticBenchmarkPanel::onStartStressTestClicked()
{
    StressTestConfig config;
    config.stress_cpu = m_stress_cpu_check->isChecked();
    config.stress_memory = m_stress_memory_check->isChecked();
    config.stress_disk = m_stress_disk_check->isChecked();
    config.duration_minutes = m_stress_duration_spin->value();
    config.thermal_limit_celsius = m_stress_thermal_limit_spin->value();

    if (!config.stress_cpu && !config.stress_memory && !config.stress_disk) {
        logMessage("Select at least one component for stress testing");
        return;
    }

    logMessage(QString("Starting stress test (%1 minutes)...")
                   .arg(config.duration_minutes));

    m_stress_test_running = true;
    m_stress_start_button->setEnabled(false);
    m_stress_stop_button->setEnabled(true);
    m_stress_status_label->setText("Status: Running");
    m_stress_status_label->setStyleSheet(QString("font-weight: 600; color: %1;")
        .arg(sak::ui::kStatusColorSuccess));

    m_controller->runStressTest(config);
}

void DiagnosticBenchmarkPanel::onStopStressTestClicked()
{
    logMessage("Stopping stress test...");
    m_controller->stopStressTest();
}

void DiagnosticBenchmarkPanel::onStressTestComplete(
    const StressTestResult& result)
{
    m_stress_test_running = false;
    m_stress_start_button->setEnabled(true);
    m_stress_stop_button->setEnabled(false);

    if (result.passed) {
        m_stress_status_label->setText("Status: PASSED");
        m_stress_status_label->setStyleSheet(QString("font-weight: 600; color: %1;")
            .arg(sak::ui::kStatusColorSuccess));
    } else {
        m_stress_status_label->setText(
            QString("Status: FAILED \u2014 %1").arg(result.abort_reason));
        m_stress_status_label->setStyleSheet(QString("font-weight: 600; color: %1;")
            .arg(sak::ui::kStatusColorError));
    }

    logMessage(QString("Stress test %1 (%2s, %3 errors, max temp: %4°C)")
                   .arg(result.passed ? "PASSED" : "FAILED")
                   .arg(result.duration_seconds)
                   .arg(result.errors_detected)
                   .arg(result.max_cpu_temp, 0, 'f', 1));
    Q_EMIT statusMessage(
        result.passed ? "Stress test passed" : "Stress test FAILED", 5000);
}

void DiagnosticBenchmarkPanel::onStressTestStatus(
    int elapsed_seconds, double cpu_temp, int errors)
{
    const int duration_sec = m_stress_duration_spin->value() * 60;
    const int percent = duration_sec > 0
                            ? (elapsed_seconds * 100 / duration_sec)
                            : 0;
    Q_EMIT progressUpdate(percent, 100);

    const int min = elapsed_seconds / 60;
    const int sec = elapsed_seconds % 60;
    m_stress_elapsed_label->setText(
        QString("Elapsed: %1:%2")
            .arg(min, 2, 10, QChar('0'))
            .arg(sec, 2, 10, QChar('0')));

    m_stress_temp_label->setText(
        QString("CPU Temp: %1°C").arg(cpu_temp, 0, 'f', 1));

    m_stress_errors_label->setText(
        QString("Errors: %1").arg(errors));

    if (errors > 0) {
        m_stress_errors_label->setStyleSheet(QString("color: %1; font-weight: 600;")
            .arg(sak::ui::kStatusColorError));
    }
}

// ============================================================================
// Slot: Full Suite
// ============================================================================

void DiagnosticBenchmarkPanel::onRunFullSuiteClicked()
{
    // Build configs from current UI state
    StressTestConfig stress_config;
    stress_config.stress_cpu = m_stress_cpu_check->isChecked();
    stress_config.stress_memory = m_stress_memory_check->isChecked();
    stress_config.stress_disk = m_stress_disk_check->isChecked();
    stress_config.duration_minutes = m_stress_duration_spin->value();
    stress_config.thermal_limit_celsius = m_stress_thermal_limit_spin->value();

    DiskBenchmarkConfig disk_config;
    if (m_disk_drive_combo->count() > 0) {
        disk_config.drive_path = m_disk_drive_combo->currentData().toString();
    } else {
        disk_config.drive_path = "C:\\";
    }

    logMessage("Starting full diagnostic suite...");
    m_suite_running = true;
    m_suite_run_button->setEnabled(false);
    m_suite_cancel_button->setEnabled(true);
    m_suite_skip_button->setEnabled(true);

    // Reset step labels
    for (int i = 0; i < 7; ++i) {
        m_suite_step_labels[i]->setStyleSheet(QString("color: %1;")
            .arg(sak::ui::kColorTextDisabled));
    }

    m_controller->runFullSuite(stress_config, disk_config);
}

void DiagnosticBenchmarkPanel::onCancelSuiteClicked()
{
    logMessage("Cancelling diagnostic suite...");
    m_controller->cancelCurrent();
    m_suite_running = false;
    m_suite_run_button->setEnabled(true);
    m_suite_cancel_button->setEnabled(false);
    m_suite_skip_button->setEnabled(false);
    m_suite_status_label->setText("Suite cancelled");
}

void DiagnosticBenchmarkPanel::onSkipStepClicked()
{
    logMessage("Skipping current suite step...");
    m_controller->skipCurrentStep();
}

void DiagnosticBenchmarkPanel::onSuiteStateChanged(
    DiagnosticController::SuiteState state)
{
    // Map state to step index
    static constexpr int state_to_step[] = {
        -1, // Idle
        0,  // HardwareScan
        1,  // SmartAnalysis
        2,  // CpuBenchmark
        3,  // DiskBenchmark
        4,  // MemoryBenchmark
        5,  // StressTest
        6,  // ReportGeneration
        -1  // Complete
    };

    const int state_idx = static_cast<int>(state);
    if (state_idx < 0 || state_idx >= static_cast<int>(std::size(state_to_step))) {
        return;  // Invalid state — ignore
    }
    const int step = state_to_step[state_idx];

    // Mark completed steps with checkmark, current with arrow icon
    for (int i = 0; i < 7; ++i) {
        if (step >= 0 && i < step) {
            // Completed — reconstruct label from stored step name
            m_suite_step_labels[i]->setText(
                QString("  %1  %2").arg(QChar(0x2705)).arg(m_suite_step_names[i]));
            m_suite_step_labels[i]->setStyleSheet(
                QString("color: %1; font-weight: 600;").arg(sak::ui::kStatusColorSuccess));
        } else if (i == step) {
            // Current \u2014 use BMP arrow symbol (U+25B6) instead of non-BMP U+1F504
            m_suite_step_labels[i]->setText(
                QString("  %1  %2").arg(QChar(0x25B6)).arg(m_suite_step_names[i]));
            m_suite_step_labels[i]->setStyleSheet(
                QString("color: %1; font-weight: 600;").arg(sak::ui::kStatusColorRunning));
        }
    }
}

void DiagnosticBenchmarkPanel::onSuiteProgress(
    int percent, const QString& message)
{
    Q_EMIT progressUpdate(percent, 100);
    m_suite_status_label->setText(message);
}

void DiagnosticBenchmarkPanel::onSuiteComplete()
{
    m_suite_running = false;
    m_suite_run_button->setEnabled(true);
    m_suite_cancel_button->setEnabled(false);
    m_suite_skip_button->setEnabled(false);
    m_suite_status_label->setText("Suite complete!");
    m_suite_status_label->setStyleSheet(QString("font-weight: 600; color: %1;")
        .arg(sak::ui::kStatusColorSuccess));

    // Mark all steps as complete \u2014 reconstruct from stored step names
    for (int i = 0; i < 7; ++i) {
        m_suite_step_labels[i]->setText(
            QString("  %1  %2").arg(QChar(0x2705)).arg(m_suite_step_names[i]));
        m_suite_step_labels[i]->setStyleSheet(
            QString("color: %1; font-weight: 600;").arg(sak::ui::kStatusColorSuccess));
    }

    logMessage("Full diagnostic suite complete");
    Q_EMIT statusMessage("Diagnostic suite complete", sak::kTimerStatusDefaultMs);
}

// ============================================================================
// Slot: Thermal Monitor
// ============================================================================

void DiagnosticBenchmarkPanel::onThermalReadingsUpdated(
    const QVector<ThermalReading>& readings)
{
    for (const auto& reading : readings) {
        const int temp = static_cast<int>(reading.temperature_celsius);

        // Color code: green < 60, yellow 60-80, red > 80
        QString color = sak::ui::kStatusColorSuccess;
        if (temp >= 80) color = sak::ui::kStatusColorError;
        else if (temp >= 60) color = sak::ui::kColorWarning;

        const QString temp_text = QString("<span style='color:%1; font-weight:600;'>"
                                          "%2°C</span>").arg(color).arg(temp);

        if (reading.component.contains("CPU", Qt::CaseInsensitive)) {
            m_thermal_cpu_label->setText(temp_text);
            m_thermal_cpu_bar->setValue(temp);
        } else if (reading.component.contains("GPU", Qt::CaseInsensitive)) {
            m_thermal_gpu_label->setText(temp_text);
            m_thermal_gpu_bar->setValue(temp);
        } else if (reading.component.contains("Disk", Qt::CaseInsensitive)) {
            m_thermal_disk_label->setText(temp_text);
            m_thermal_disk_bar->setValue(temp);
        }
    }
}

// ============================================================================
// Slot: Report Generation
// ============================================================================

void DiagnosticBenchmarkPanel::onGenerateReportClicked()
{
    const auto dir = QFileDialog::getExistingDirectory(
        this, "Select Report Output Directory");
    if (dir.isEmpty()) return;

    logMessage(QString("Generating reports in %1...").arg(dir));
    m_controller->generateReport(
        dir,
        m_report_technician_edit->text(),
        m_report_ticket_edit->text(),
        m_report_notes_edit->toPlainText(),
        "html,json,csv");
}

void DiagnosticBenchmarkPanel::onReportsGenerated(const QString& output_dir)
{
    logMessage(QString("Reports generated in %1").arg(output_dir));
    Q_EMIT statusMessage("Reports saved to " + output_dir, sak::kTimerStatusDefaultMs);

    QMessageBox::information(this, "Reports Generated",
        QString("Diagnostic reports saved to:\n%1").arg(output_dir));
}

// ============================================================================
// Slot: Progress & Error
// ============================================================================

void DiagnosticBenchmarkPanel::onOperationProgress(
    int percent, const QString& message)
{
    Q_EMIT progressUpdate(percent, 100);
    Q_EMIT statusMessage(message, 0);
}

void DiagnosticBenchmarkPanel::onErrorOccurred(const QString& message)
{
    // Note: Do NOT call setOperationRunning(false) here.
    // SmartDiskAnalyzer emits non-fatal per-drive errors while analysis
    // continues. The completion handler takes care of re-enabling buttons.

    // Reset suite state if suite was running
    if (m_suite_running) {
        // Suite errors are handled by the controller's advanceSuiteStep;
        // only log here.
    }

    logMessage(QString("ERROR: %1").arg(message));
    Q_EMIT statusMessage("Error: " + message, sak::kTimerStatusDefaultMs);
}

} // namespace sak
