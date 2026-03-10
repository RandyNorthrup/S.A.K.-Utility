// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_types.cpp
/// @brief Unit tests for diagnostic data structures â€” defaults, construction, value semantics

#include "sak/diagnostic_types.h"

#include <QtTest/QtTest>

using namespace sak;

class DiagnosticTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cpuBenchmarkResult_defaults();
    void diskBenchmarkConfig_defaults();
    void diskBenchmarkResult_defaults();
    void memoryBenchmarkResult_defaults();
    void stressTestConfig_defaults();
    void stressTestResult_defaults();
    void thermalReading_construction();
    void hardwareInventory_defaults();
    void smartReport_defaults();
    void reportData_defaults();
    void diagnosticStatus_enum();
    void smartHealthStatus_enum();
};

void DiagnosticTypesTests::cpuBenchmarkResult_defaults() {
    CpuBenchmarkResult r;
    QCOMPARE(r.prime_sieve_time_ms, 0.0);
    QCOMPARE(r.matrix_multiply_time_ms, 0.0);
    QCOMPARE(r.single_thread_score, 0);
    QCOMPARE(r.multi_thread_score, 0);
    QCOMPARE(r.thread_count, 0u);
    QVERIFY(!r.thermal_throttle_detected);
}

void DiagnosticTypesTests::diskBenchmarkConfig_defaults() {
    DiskBenchmarkConfig c;
    QVERIFY(c.drive_path.isEmpty());
    QCOMPARE(c.test_file_size_mb, uint64_t(1024));
    QCOMPARE(c.queue_depth_low, 1);
    QCOMPARE(c.queue_depth_high, 32);
    QCOMPARE(c.sequential_passes, 3);
    QCOMPARE(c.random_duration_sec, 30);
}

void DiagnosticTypesTests::diskBenchmarkResult_defaults() {
    DiskBenchmarkResult r;
    QCOMPARE(r.seq_read_mbps, 0.0);
    QCOMPARE(r.seq_write_mbps, 0.0);
    QCOMPARE(r.p99_read_latency_us, 0.0);
    QCOMPARE(r.p99_write_latency_us, 0.0);
    QCOMPARE(r.overall_score, 0);
}

void DiagnosticTypesTests::memoryBenchmarkResult_defaults() {
    MemoryBenchmarkResult r;
    QCOMPARE(r.read_bandwidth_gbps, 0.0);
    QCOMPARE(r.write_bandwidth_gbps, 0.0);
    QCOMPARE(r.random_latency_ns, 0.0);
    QCOMPARE(r.overall_score, 0);
}

void DiagnosticTypesTests::stressTestConfig_defaults() {
    StressTestConfig c;
    QVERIFY(c.stress_cpu);
    QVERIFY(c.stress_memory);
    QVERIFY(!c.stress_disk);
    QCOMPARE(c.duration_minutes, 10);
    QCOMPARE(c.cpu_threads, 0);
    QCOMPARE(c.thermal_limit_celsius, 95.0);
    QVERIFY(c.abort_on_error);
}

void DiagnosticTypesTests::stressTestResult_defaults() {
    StressTestResult r;
    QVERIFY(!r.passed);
    QCOMPARE(r.duration_seconds, 0);
    QCOMPARE(r.errors_detected, 0);
    QCOMPARE(r.memory_pattern_errors, 0);
    QCOMPARE(r.thermal_throttle_events, 0);
    QVERIFY(r.abort_reason.isEmpty());
}

void DiagnosticTypesTests::thermalReading_construction() {
    ThermalReading reading;
    reading.component = "CPU Package";
    reading.temperature_celsius = 65.5;
    reading.timestamp = QDateTime::currentDateTime();

    QCOMPARE(reading.component, QString("CPU Package"));
    QCOMPARE(reading.temperature_celsius, 65.5);
    QVERIFY(reading.timestamp.isValid());
}

void DiagnosticTypesTests::hardwareInventory_defaults() {
    HardwareInventory inv;
    QVERIFY(inv.cpu.name.isEmpty());
    QCOMPARE(inv.cpu.cores, 0u);
    QCOMPARE(inv.memory.total_bytes, uint64_t(0));
    QVERIFY(inv.storage.isEmpty());
    QVERIFY(inv.gpus.isEmpty());
    QVERIFY(!inv.battery.present);
    QCOMPARE(inv.uptime_seconds, uint64_t(0));
}

void DiagnosticTypesTests::smartReport_defaults() {
    SmartReport r;
    QCOMPARE(r.overall_health, SmartHealthStatus::Unknown);
    QCOMPARE(r.power_on_hours, int64_t(0));
    QCOMPARE(r.reallocated_sectors, int64_t(0));
    QVERIFY(!r.nvme_health.has_value());
    QVERIFY(r.warnings.isEmpty());
    QVERIFY(r.recommendations.isEmpty());
}

void DiagnosticTypesTests::reportData_defaults() {
    DiagnosticReportData d;
    QCOMPARE(d.overall_status, DiagnosticStatus::AllPassed);
    QVERIFY(d.critical_issues.isEmpty());
    QVERIFY(d.warnings.isEmpty());
    QVERIFY(d.recommendations.isEmpty());
    QVERIFY(!d.cpu_benchmark.has_value());
    QVERIFY(!d.disk_benchmark.has_value());
    QVERIFY(!d.memory_benchmark.has_value());
    QVERIFY(!d.stress_test.has_value());
}

void DiagnosticTypesTests::diagnosticStatus_enum() {
    QVERIFY(DiagnosticStatus::AllPassed != DiagnosticStatus::Warnings);
    QVERIFY(DiagnosticStatus::Warnings != DiagnosticStatus::CriticalIssues);
}

void DiagnosticTypesTests::smartHealthStatus_enum() {
    QVERIFY(SmartHealthStatus::Healthy != SmartHealthStatus::Warning);
    QVERIFY(SmartHealthStatus::Warning != SmartHealthStatus::Critical);
    QVERIFY(SmartHealthStatus::Critical != SmartHealthStatus::Unknown);
}

QTEST_MAIN(DiagnosticTypesTests)
#include "test_diagnostic_types.moc"
