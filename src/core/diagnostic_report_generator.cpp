// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_report_generator.cpp
/// @brief Diagnostic report generation implementation (HTML, JSON, CSV)

#include "sak/diagnostic_report_generator.h"
#include "sak/format_utils.h"
#include "sak/logger.h"

#include <QtGlobal>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace sak {

namespace {

/// @brief Escape a value for CSV output (RFC 4180 compliant)
/// If the value contains commas, quotes, or newlines, wrap in quotes and
/// double any embedded quotes.
QString csvEscape(const QString& value)
{
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"')) ||
        value.contains(QLatin1Char('\n'))) {
        QString escaped = value;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    return value;
}

} // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

DiagnosticReportGenerator::DiagnosticReportGenerator(QObject* parent)
    : QObject(parent)
{
}

// ============================================================================
// Public API
// ============================================================================

bool DiagnosticReportGenerator::generateHtml(const QString& output_path)
{
    Q_ASSERT_X(!output_path.isEmpty(), "generateHtml", "output_path must not be empty");
    const QString html = renderHtml();
    if (html.isEmpty()) {
        Q_EMIT errorOccurred("Failed to render HTML report");
        return false;
    }

    // Ensure output directory exists
    QDir().mkpath(QFileInfo(output_path).absolutePath());

    QFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QString err = QString("Failed to open %1 for writing: %2")
                                .arg(output_path, file.errorString());
        Q_EMIT errorOccurred(err);
        logError("{}", err.toStdString());
        return false;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << html;
    file.close();

    logInfo("HTML report generated: {}", output_path.toStdString());
    Q_EMIT reportGenerated("HTML", output_path);
    return true;
}

bool DiagnosticReportGenerator::generateJson(const QString& output_path)
{
    Q_ASSERT_X(!output_path.isEmpty(), "generateJson", "output_path must not be empty");
    QJsonObject root;

    // Metadata
    QJsonObject meta;
    meta["generated_at"] = m_data.report_timestamp.toString(Qt::ISODate);
    meta["technician"]   = m_data.technician_name;
    meta["ticket"]       = m_data.ticket_number;
    meta["notes"]        = m_data.notes;
    meta["overall_status"] = (m_data.overall_status == DiagnosticStatus::AllPassed)
                                 ? "PASSED"
                                 : (m_data.overall_status == DiagnosticStatus::Warnings)
                                       ? "WARNINGS"
                                       : "CRITICAL";
    root["metadata"] = meta;

    // Hardware inventory
    QJsonObject hw;
    const auto& inv = m_data.inventory;

    // CPU
    QJsonObject cpu;
    cpu["name"]         = inv.cpu.name;
    cpu["manufacturer"] = inv.cpu.manufacturer;
    cpu["cores"]        = static_cast<int>(inv.cpu.cores);
    cpu["threads"]      = static_cast<int>(inv.cpu.threads);
    cpu["base_clock_mhz"]  = static_cast<int>(inv.cpu.base_clock_mhz);
    cpu["max_clock_mhz"]   = static_cast<int>(inv.cpu.max_clock_mhz);
    cpu["l2_cache_kb"]     = static_cast<int>(inv.cpu.l2_cache_kb);
    cpu["l3_cache_kb"]     = static_cast<int>(inv.cpu.l3_cache_kb);
    cpu["architecture"]    = inv.cpu.architecture;
    hw["cpu"] = cpu;

    // Memory
    QJsonObject mem;
    mem["total_gb"] = static_cast<double>(inv.memory.total_bytes) / (1024.0 * 1024 * 1024);
    mem["slots_used"]  = static_cast<int>(inv.memory.slots_used);
    mem["slots_total"] = static_cast<int>(inv.memory.slots_total);

    QJsonArray modules;
    for (const auto& mod : inv.memory.modules) {
        QJsonObject m;
        m["manufacturer"] = mod.manufacturer;
        m["part_number"]  = mod.part_number;
        m["capacity_gb"]  = static_cast<double>(mod.capacity_bytes) / (1024.0 * 1024 * 1024);
        m["speed_mhz"]    = static_cast<int>(mod.speed_mhz);
        m["type"]         = mod.memory_type;
        m["form_factor"]  = mod.form_factor;
        modules.append(m);
    }
    mem["modules"] = modules;
    hw["memory"] = mem;

    // Storage
    QJsonArray storage_arr;
    for (const auto& dev : inv.storage) {
        QJsonObject d;
        d["model"]       = dev.model;
        d["serial"]      = dev.serial_number;
        d["size_gb"]     = static_cast<double>(dev.size_bytes) / (1024.0 * 1024 * 1024);
        d["interface"]   = dev.interface_type;
        d["media_type"]  = dev.media_type;
        d["firmware"]    = dev.firmware_version;
        storage_arr.append(d);
    }
    hw["storage"] = storage_arr;

    // GPUs
    QJsonArray gpu_arr;
    for (const auto& gpu : inv.gpus) {
        QJsonObject g;
        g["name"]           = gpu.name;
        g["manufacturer"]   = gpu.manufacturer;
        g["vram_gb"]        = static_cast<double>(gpu.vram_bytes) / (1024.0 * 1024 * 1024);
        g["driver_version"] = gpu.driver_version;
        gpu_arr.append(g);
    }
    hw["gpus"] = gpu_arr;

    // OS
    QJsonObject os;
    os["name"]         = inv.os_name;
    os["version"]      = inv.os_version;
    os["build"]        = inv.os_build;
    os["architecture"] = inv.os_architecture;
    hw["os"] = os;

    root["hardware"] = hw;

    // SMART reports
    QJsonArray smart_arr;
    for (const auto& report : m_data.smart_reports) {
        QJsonObject s;
        s["device"]    = report.device_path;
        s["model"]     = report.model;
        s["serial"]    = report.serial_number;
        s["status"]    = healthStatusText(report.overall_health);
        s["temp_c"]    = report.temperature_celsius;
        s["power_on_hours"] = static_cast<qint64>(report.power_on_hours);

        QJsonArray warnings;
        for (const auto& w : report.warnings) warnings.append(w);
        s["warnings"] = warnings;

        smart_arr.append(s);
    }
    root["smart"] = smart_arr;

    // Benchmarks
    QJsonObject benchmarks;
    if (m_data.cpu_benchmark.has_value()) {
        const auto& cb = m_data.cpu_benchmark.value();
        QJsonObject cpu_bench;
        cpu_bench["single_thread_score"] = cb.single_thread_score;
        cpu_bench["multi_thread_score"]  = cb.multi_thread_score;
        cpu_bench["thread_scaling"]      = cb.thread_scaling_efficiency;
        cpu_bench["prime_sieve_ms"]      = cb.prime_sieve_time_ms;
        cpu_bench["matrix_multiply_ms"]  = cb.matrix_multiply_time_ms;
        cpu_bench["zlib_mbps"]           = cb.zlib_throughput_mbps;
        cpu_bench["aes_mbps"]            = cb.aes_throughput_mbps;
        benchmarks["cpu"] = cpu_bench;
    }

    if (m_data.disk_benchmark.has_value()) {
        const auto& db = m_data.disk_benchmark.value();
        QJsonObject disk_bench;
        disk_bench["drive"]            = db.drive_path;
        disk_bench["seq_read_mbps"]    = db.seq_read_mbps;
        disk_bench["seq_write_mbps"]   = db.seq_write_mbps;
        disk_bench["rand_4k_read_iops"]  = db.rand_4k_read_iops;
        disk_bench["rand_4k_write_iops"] = db.rand_4k_write_iops;
        disk_bench["score"]            = db.overall_score;
        benchmarks["disk"] = disk_bench;
    }

    if (m_data.memory_benchmark.has_value()) {
        const auto& mb = m_data.memory_benchmark.value();
        QJsonObject mem_bench;
        mem_bench["read_gbps"]     = mb.read_bandwidth_gbps;
        mem_bench["write_gbps"]    = mb.write_bandwidth_gbps;
        mem_bench["copy_gbps"]     = mb.copy_bandwidth_gbps;
        mem_bench["latency_ns"]    = mb.random_latency_ns;
        mem_bench["score"]         = mb.overall_score;
        benchmarks["memory"] = mem_bench;
    }
    root["benchmarks"] = benchmarks;

    // Stress test
    if (m_data.stress_test.has_value()) {
        const auto& st = m_data.stress_test.value();
        QJsonObject stress;
        stress["passed"]           = st.passed;
        stress["duration_seconds"] = st.duration_seconds;
        stress["errors"]           = st.errors_detected;
        stress["max_cpu_temp"]     = st.max_cpu_temp;
        stress["throttle_events"]  = st.thermal_throttle_events;
        stress["abort_reason"]     = st.abort_reason;
        root["stress_test"] = stress;
    }

    // Issues and recommendations
    QJsonArray critical_arr, warn_arr, rec_arr;
    for (const auto& c : m_data.critical_issues) critical_arr.append(c);
    for (const auto& w : m_data.warnings) warn_arr.append(w);
    for (const auto& r : m_data.recommendations) rec_arr.append(r);
    root["critical_issues"]  = critical_arr;
    root["warnings"]         = warn_arr;
    root["recommendations"]  = rec_arr;

    // Write to file
    QDir().mkpath(QFileInfo(output_path).absolutePath());
    QFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QString("Failed to open %1: %2").arg(output_path, file.errorString()));
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    logInfo("JSON report generated: {}", output_path.toStdString());
    Q_EMIT reportGenerated("JSON", output_path);
    return true;
}

bool DiagnosticReportGenerator::generateCsv(const QString& output_path)
{
    Q_ASSERT_X(!output_path.isEmpty(), "generateCsv", "output_path must not be empty");
    QDir().mkpath(QFileInfo(output_path).absolutePath());
    QFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QString("Failed to open %1: %2").arg(output_path, file.errorString()));
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // Section: Hardware Summary
    out << "Section,Property,Value\n";
    out << "CPU,Name," << csvEscape(m_data.inventory.cpu.name) << "\n";
    out << "CPU,Cores," << m_data.inventory.cpu.cores << "\n";
    out << "CPU,Threads," << m_data.inventory.cpu.threads << "\n";
    out << "CPU,Base Clock (MHz)," << m_data.inventory.cpu.base_clock_mhz << "\n";
    out << "CPU,Max Clock (MHz)," << m_data.inventory.cpu.max_clock_mhz << "\n";

    out << "Memory,Total (GB),"
        << QString::number(static_cast<double>(m_data.inventory.memory.total_bytes) /
                           (1024.0 * 1024 * 1024), 'f', 1)
        << "\n";
    out << "Memory,Slots Used," << m_data.inventory.memory.slots_used << "\n";
    out << "Memory,Slots Total," << m_data.inventory.memory.slots_total << "\n";

    for (const auto& dev : m_data.inventory.storage) {
        out << "Storage,Model," << csvEscape(dev.model) << "\n";
        out << "Storage,Size (GB),"
            << QString::number(static_cast<double>(dev.size_bytes) / (1024.0 * 1024 * 1024), 'f', 1)
            << "\n";
        out << "Storage,Interface," << csvEscape(dev.interface_type) << "\n";
        out << "Storage,Media Type," << csvEscape(dev.media_type) << "\n";
    }

    for (const auto& gpu : m_data.inventory.gpus) {
        out << "GPU,Name," << csvEscape(gpu.name) << "\n";
        out << "GPU,VRAM (GB),"
            << QString::number(static_cast<double>(gpu.vram_bytes) / (1024.0 * 1024 * 1024), 'f', 1)
            << "\n";
        out << "GPU,Driver," << csvEscape(gpu.driver_version) << "\n";
    }

    out << "OS,Name," << csvEscape(m_data.inventory.os_name) << "\n";
    out << "OS,Build," << csvEscape(m_data.inventory.os_build) << "\n";

    // Section: SMART Health
    out << "\nSMART Health\n";
    out << "Device,Model,Status,Temperature (C),Power-On Hours\n";
    for (const auto& report : m_data.smart_reports) {
        out << csvEscape(report.device_path) << ","
            << csvEscape(report.model) << ","
            << healthStatusText(report.overall_health) << ","
            << report.temperature_celsius << ","
            << report.power_on_hours << "\n";
    }

    // Section: Benchmarks
    out << "\nBenchmark Results\n";
    if (m_data.cpu_benchmark.has_value()) {
        const auto& cb = m_data.cpu_benchmark.value();
        out << "CPU Benchmark,Single Thread Score," << cb.single_thread_score << "\n";
        out << "CPU Benchmark,Multi Thread Score," << cb.multi_thread_score << "\n";
        out << "CPU Benchmark,ZLIB Throughput (MB/s)," << cb.zlib_throughput_mbps << "\n";
    }

    if (m_data.disk_benchmark.has_value()) {
        const auto& db = m_data.disk_benchmark.value();
        out << "Disk Benchmark,Sequential Read (MB/s)," << db.seq_read_mbps << "\n";
        out << "Disk Benchmark,Sequential Write (MB/s)," << db.seq_write_mbps << "\n";
        out << "Disk Benchmark,Random 4K Read  (IOPS)," << db.rand_4k_read_iops << "\n";
        out << "Disk Benchmark,Random 4K Write (IOPS)," << db.rand_4k_write_iops << "\n";
        out << "Disk Benchmark,Score," << db.overall_score << "\n";
    }

    if (m_data.memory_benchmark.has_value()) {
        const auto& mb = m_data.memory_benchmark.value();
        out << "Memory Benchmark,Read (GB/s)," << mb.read_bandwidth_gbps << "\n";
        out << "Memory Benchmark,Write (GB/s)," << mb.write_bandwidth_gbps << "\n";
        out << "Memory Benchmark,Latency (ns)," << mb.random_latency_ns << "\n";
        out << "Memory Benchmark,Score," << mb.overall_score << "\n";
    }

    // Stress test
    if (m_data.stress_test.has_value()) {
        const auto& st = m_data.stress_test.value();
        out << "Stress Test,Passed," << (st.passed ? "Yes" : "No") << "\n";
        out << "Stress Test,Duration (s)," << st.duration_seconds << "\n";
        out << "Stress Test,Errors," << st.errors_detected << "\n";
        out << "Stress Test,Max CPU Temp (C)," << st.max_cpu_temp << "\n";
    }

    file.close();
    logInfo("CSV report generated: {}", output_path.toStdString());
    Q_EMIT reportGenerated("CSV", output_path);
    return true;
}

// ============================================================================
// HTML Rendering
// ============================================================================

QString DiagnosticReportGenerator::renderHtml() const
{
    QString html;
    html.reserve(32768);

    html += buildHtmlHeader();
    html += "<body>\n";
    html += "<div class=\"container\">\n";
    html += "<h1>S.A.K. Utility — Diagnostic Report</h1>\n";

    // Metadata bar
    html += "<div class=\"meta-bar\">\n";
    html += QString("<span><strong>Date:</strong> %1</span>\n")
                .arg(m_data.report_timestamp.toString("yyyy-MM-dd hh:mm:ss"));
    if (!m_data.technician_name.isEmpty()) {
        html += QString("<span><strong>Technician:</strong> %1</span>\n")
                    .arg(m_data.technician_name.toHtmlEscaped());
    }
    if (!m_data.ticket_number.isEmpty()) {
        html += QString("<span><strong>Ticket:</strong> %1</span>\n")
                    .arg(m_data.ticket_number.toHtmlEscaped());
    }
    html += "</div>\n";

    // Overall status banner
    const QString status_class = (m_data.overall_status == DiagnosticStatus::AllPassed)
                                     ? "status-healthy"
                                     : (m_data.overall_status == DiagnosticStatus::Warnings)
                                           ? "status-warning"
                                           : "status-critical";
    const QString status_text = (m_data.overall_status == DiagnosticStatus::AllPassed)
                                    ? "ALL TESTS PASSED"
                                    : (m_data.overall_status == DiagnosticStatus::Warnings)
                                          ? "WARNINGS DETECTED"
                                          : "CRITICAL ISSUES FOUND";
    html += QString("<div class=\"overall-status %1\">%2</div>\n")
                .arg(status_class, status_text);

    html += buildHardwareSection();
    html += buildSmartSection();
    html += buildBenchmarkSection();
    html += buildStressTestSection();
    html += buildRecommendationsSection();

    // Notes
    if (!m_data.notes.isEmpty()) {
        html += "<h2>Notes</h2>\n";
        html += QString("<p>%1</p>\n").arg(m_data.notes.toHtmlEscaped());
    }

    html += "<div class=\"footer\">Generated by S.A.K. Utility — Swiss Army Knife for IT</div>\n";
    html += "</div>\n</body>\n</html>";

    return html;
}

QString DiagnosticReportGenerator::buildHtmlHeader() const
{
    return R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>S.A.K. Diagnostic Report</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #f5f5f5; color: #333; line-height: 1.6; }
  .container { max-width: 900px; margin: 20px auto; background: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
  h1 { color: #1a237e; margin-bottom: 10px; font-size: 24px; }
  h2 { color: #283593; margin: 25px 0 10px; padding-bottom: 5px; border-bottom: 2px solid #e8eaf6; font-size: 18px; }
  .meta-bar { display: flex; gap: 20px; flex-wrap: wrap; margin: 10px 0 20px; padding: 10px; background: #e8eaf6; border-radius: 4px; font-size: 14px; }
  .overall-status { text-align: center; padding: 15px; border-radius: 6px; font-weight: bold; font-size: 18px; margin: 15px 0; color: #fff; }
  .status-healthy { background: #2e7d32; }
  .status-warning { background: #f57f17; }
  .status-critical { background: #c62828; }
  table { width: 100%; border-collapse: collapse; margin: 10px 0; font-size: 14px; }
  th { background: #3949ab; color: #fff; padding: 8px 12px; text-align: left; }
  td { padding: 6px 12px; border-bottom: 1px solid #e0e0e0; }
  tr:nth-child(even) td { background: #f9f9ff; }
  .badge { display: inline-block; padding: 2px 8px; border-radius: 3px; font-size: 12px; font-weight: bold; color: #fff; }
  .badge-healthy { background: #2e7d32; }
  .badge-warning { background: #f57f17; }
  .badge-critical { background: #c62828; }
  .badge-unknown { background: #757575; }
  .score { font-size: 28px; font-weight: bold; color: #1a237e; }
  .rec-list { margin: 10px 0; padding-left: 20px; }
  .rec-list li { margin: 5px 0; }
  .rec-critical { color: #c62828; font-weight: bold; }
  .rec-warning { color: #f57f17; }
  .footer { text-align: center; margin-top: 30px; padding-top: 15px; border-top: 1px solid #e0e0e0; color: #9e9e9e; font-size: 12px; }
  @media print { body { background: #fff; } .container { box-shadow: none; margin: 0; padding: 15px; } }
</style>
</head>
)";
}

QString DiagnosticReportGenerator::buildHardwareSection() const
{
    const auto& inv = m_data.inventory;
    QString html;

    html += "<h2>Hardware Inventory</h2>\n";
    html += "<table>\n";
    html += "<tr><th colspan=\"2\">CPU</th></tr>\n";
    html += QString("<tr><td>Model</td><td>%1</td></tr>\n").arg(inv.cpu.name.toHtmlEscaped());
    html += QString("<tr><td>Cores / Threads</td><td>%1 / %2</td></tr>\n")
                .arg(inv.cpu.cores).arg(inv.cpu.threads);
    html += QString("<tr><td>Clock Speed</td><td>%1 MHz (max %2 MHz)</td></tr>\n")
                .arg(inv.cpu.base_clock_mhz).arg(inv.cpu.max_clock_mhz);
    html += QString("<tr><td>Cache</td><td>L2: %1 KB | L3: %2 KB</td></tr>\n")
                .arg(inv.cpu.l2_cache_kb).arg(inv.cpu.l3_cache_kb);

    html += "<tr><th colspan=\"2\">Memory</th></tr>\n";
    html += QString("<tr><td>Total</td><td>%1</td></tr>\n")
                .arg(formatBytes(inv.memory.total_bytes));
    html += QString("<tr><td>Slots</td><td>%1 of %2 used</td></tr>\n")
                .arg(inv.memory.slots_used).arg(inv.memory.slots_total);

    for (const auto& mod : inv.memory.modules) {
        html += QString("<tr><td>Slot %1</td><td>%2 %3 %4 @ %5 MHz</td></tr>\n")
                    .arg(mod.slot)
                    .arg(mod.manufacturer.toHtmlEscaped())
                    .arg(formatBytes(mod.capacity_bytes))
                    .arg(mod.memory_type)
                    .arg(mod.speed_mhz);
    }

    html += "<tr><th colspan=\"2\">Storage</th></tr>\n";
    for (const auto& dev : inv.storage) {
        html += QString("<tr><td>%1</td><td>%2 (%3, %4)</td></tr>\n")
                    .arg(dev.model.toHtmlEscaped())
                    .arg(formatBytes(dev.size_bytes))
                    .arg(dev.interface_type)
                    .arg(dev.media_type);
    }

    html += "<tr><th colspan=\"2\">GPU</th></tr>\n";
    for (const auto& gpu : inv.gpus) {
        html += QString("<tr><td>%1</td><td>%2 VRAM, Driver %3</td></tr>\n")
                    .arg(gpu.name.toHtmlEscaped())
                    .arg(formatBytes(gpu.vram_bytes))
                    .arg(gpu.driver_version);
    }

    html += "<tr><th colspan=\"2\">System</th></tr>\n";
    html += QString("<tr><td>OS</td><td>%1 (Build %2)</td></tr>\n")
                .arg(inv.os_name.toHtmlEscaped()).arg(inv.os_build);
    html += QString("<tr><td>Motherboard</td><td>%1 %2</td></tr>\n")
                .arg(inv.motherboard.manufacturer.toHtmlEscaped())
                .arg(inv.motherboard.product.toHtmlEscaped());

    html += "</table>\n";
    return html;
}

QString DiagnosticReportGenerator::buildSmartSection() const
{
    if (m_data.smart_reports.isEmpty()) return {};

    QString html;
    html += "<h2>Disk Health (SMART)</h2>\n";
    html += "<table>\n";
    html += "<tr><th>Drive</th><th>Model</th><th>Health</th><th>Temp</th><th>Power-On</th></tr>\n";

    for (const auto& report : m_data.smart_reports) {
        const QString badge_class = "badge " + healthStatusCssClass(report.overall_health);
        html += QString("<tr><td>%1</td><td>%2</td>"
                        "<td><span class=\"%3\">%4</span></td>"
                        "<td>%5°C</td><td>%6 hrs</td></tr>\n")
                    .arg(report.device_path.toHtmlEscaped())
                    .arg(report.model.toHtmlEscaped())
                    .arg(badge_class)
                    .arg(healthStatusText(report.overall_health))
                    .arg(report.temperature_celsius, 0, 'f', 0)
                    .arg(report.power_on_hours);
    }

    html += "</table>\n";

    // Warnings from SMART
    for (const auto& report : m_data.smart_reports) {
        if (!report.warnings.isEmpty()) {
            html += QString("<p><strong>%1:</strong></p>\n<ul>\n")
                        .arg(report.model.toHtmlEscaped());
            for (const auto& w : report.warnings) {
                html += QString("<li>%1</li>\n").arg(w.toHtmlEscaped());
            }
            html += "</ul>\n";
        }
    }

    return html;
}

QString DiagnosticReportGenerator::buildBenchmarkSection() const
{
    QString html;

    bool has_any = m_data.cpu_benchmark.has_value() ||
                   m_data.disk_benchmark.has_value() ||
                   m_data.memory_benchmark.has_value();
    if (!has_any) return {};

    html += "<h2>Benchmark Results</h2>\n";

    if (m_data.cpu_benchmark.has_value()) {
        const auto& cb = m_data.cpu_benchmark.value();
        html += "<h3>CPU Performance</h3>\n";
        html += "<table>\n";
        html += QString("<tr><td>Single-Thread Score</td><td class=\"score\">%1</td></tr>\n")
                    .arg(cb.single_thread_score);
        html += QString("<tr><td>Multi-Thread Score</td><td class=\"score\">%1</td></tr>\n")
                    .arg(cb.multi_thread_score);
        html += QString("<tr><td>Thread Scaling</td><td>%1%</td></tr>\n")
                    .arg(cb.thread_scaling_efficiency * 100.0, 0, 'f', 1);
        html += QString("<tr><td>ZLIB Throughput</td><td>%1 MB/s</td></tr>\n")
                    .arg(cb.zlib_throughput_mbps, 0, 'f', 1);
        html += QString("<tr><td>AES Throughput</td><td>%1 MB/s</td></tr>\n")
                    .arg(cb.aes_throughput_mbps, 0, 'f', 1);
        html += "</table>\n";
    }

    if (m_data.disk_benchmark.has_value()) {
        const auto& db = m_data.disk_benchmark.value();
        html += "<h3>Disk I/O Performance</h3>\n";
        html += "<table>\n";
        html += QString("<tr><td>Drive</td><td>%1</td></tr>\n").arg(db.drive_path.toHtmlEscaped());
        html += QString("<tr><td>Sequential Read</td><td>%1 MB/s</td></tr>\n")
                    .arg(db.seq_read_mbps, 0, 'f', 0);
        html += QString("<tr><td>Sequential Write</td><td>%1 MB/s</td></tr>\n")
                    .arg(db.seq_write_mbps, 0, 'f', 0);
        html += QString("<tr><td>Random 4K Read (QD1)</td><td>%1 IOPS</td></tr>\n")
                    .arg(db.rand_4k_read_iops, 0, 'f', 0);
        html += QString("<tr><td>Random 4K Write (QD1)</td><td>%1 IOPS</td></tr>\n")
                    .arg(db.rand_4k_write_iops, 0, 'f', 0);
        html += QString("<tr><td>Score</td><td class=\"score\">%1</td></tr>\n")
                    .arg(db.overall_score);
        html += "</table>\n";
    }

    if (m_data.memory_benchmark.has_value()) {
        const auto& mb = m_data.memory_benchmark.value();
        html += "<h3>Memory Performance</h3>\n";
        html += "<table>\n";
        html += QString("<tr><td>Read Bandwidth</td><td>%1 GB/s</td></tr>\n")
                    .arg(mb.read_bandwidth_gbps, 0, 'f', 1);
        html += QString("<tr><td>Write Bandwidth</td><td>%1 GB/s</td></tr>\n")
                    .arg(mb.write_bandwidth_gbps, 0, 'f', 1);
        html += QString("<tr><td>Copy Bandwidth</td><td>%1 GB/s</td></tr>\n")
                    .arg(mb.copy_bandwidth_gbps, 0, 'f', 1);
        html += QString("<tr><td>Random Latency</td><td>%1 ns</td></tr>\n")
                    .arg(mb.random_latency_ns, 0, 'f', 1);
        html += QString("<tr><td>Score</td><td class=\"score\">%1</td></tr>\n")
                    .arg(mb.overall_score);
        html += "</table>\n";
    }

    return html;
}

QString DiagnosticReportGenerator::buildStressTestSection() const
{
    if (!m_data.stress_test.has_value()) return {};

    const auto& st = m_data.stress_test.value();
    QString html;

    html += "<h2>Stress Test Results</h2>\n";

    const QString result_class = st.passed ? "status-healthy" : "status-critical";
    const QString result_text  = st.passed ? "PASSED" : "FAILED";
    html += QString("<div class=\"overall-status %1\">Stress Test: %2</div>\n")
                .arg(result_class, result_text);

    html += "<table>\n";
    html += QString("<tr><td>Duration</td><td>%1 seconds</td></tr>\n").arg(st.duration_seconds);
    html += QString("<tr><td>Errors Detected</td><td>%1</td></tr>\n").arg(st.errors_detected);
    html += QString("<tr><td>Max CPU Temperature</td><td>%1°C</td></tr>\n")
                .arg(st.max_cpu_temp, 0, 'f', 1);
    html += QString("<tr><td>Thermal Throttle Events</td><td>%1</td></tr>\n")
                .arg(st.thermal_throttle_events);

    if (!st.abort_reason.isEmpty()) {
        html += QString("<tr><td>Abort Reason</td><td><strong>%1</strong></td></tr>\n")
                    .arg(st.abort_reason.toHtmlEscaped());
    }

    html += "</table>\n";
    return html;
}

QString DiagnosticReportGenerator::buildRecommendationsSection() const
{
    if (m_data.critical_issues.isEmpty() &&
        m_data.warnings.isEmpty() &&
        m_data.recommendations.isEmpty()) {
        return {};
    }

    QString html;
    html += "<h2>Findings &amp; Recommendations</h2>\n";

    if (!m_data.critical_issues.isEmpty()) {
        html += "<h3>Critical Issues</h3>\n<ul class=\"rec-list\">\n";
        for (const auto& issue : m_data.critical_issues) {
            html += QString("<li class=\"rec-critical\">%1</li>\n").arg(issue.toHtmlEscaped());
        }
        html += "</ul>\n";
    }

    if (!m_data.warnings.isEmpty()) {
        html += "<h3>Warnings</h3>\n<ul class=\"rec-list\">\n";
        for (const auto& warning : m_data.warnings) {
            html += QString("<li class=\"rec-warning\">%1</li>\n").arg(warning.toHtmlEscaped());
        }
        html += "</ul>\n";
    }

    if (!m_data.recommendations.isEmpty()) {
        html += "<h3>Recommendations</h3>\n<ul class=\"rec-list\">\n";
        for (const auto& rec : m_data.recommendations) {
            html += QString("<li>%1</li>\n").arg(rec.toHtmlEscaped());
        }
        html += "</ul>\n";
    }

    return html;
}

// ============================================================================
// Helpers
// ============================================================================

QString DiagnosticReportGenerator::healthStatusCssClass(SmartHealthStatus status)
{
    switch (status) {
        case SmartHealthStatus::Healthy:  return "badge-healthy";
        case SmartHealthStatus::Warning:  return "badge-warning";
        case SmartHealthStatus::Critical: return "badge-critical";
        case SmartHealthStatus::Unknown:  return "badge-unknown";
    }
    return "badge-unknown";
}

QString DiagnosticReportGenerator::healthStatusText(SmartHealthStatus status)
{
    switch (status) {
        case SmartHealthStatus::Healthy:  return "Healthy";
        case SmartHealthStatus::Warning:  return "Warning";
        case SmartHealthStatus::Critical: return "Critical";
        case SmartHealthStatus::Unknown:  return "Unknown";
    }
    return "Unknown";
}

QString DiagnosticReportGenerator::formatBytes(uint64_t bytes)
{
    return sak::formatBytes(bytes);
}

} // namespace sak
