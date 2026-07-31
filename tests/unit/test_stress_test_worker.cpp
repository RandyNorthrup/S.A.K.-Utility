// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_stress_test_worker.cpp
/// @brief Unit tests for StressTestWorker construction, config, result defaults

#include "sak/stress_test_worker.h"

#include <QtTest/QtTest>

using namespace sak;

class TestStressTestWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_isWorkerBase();
    void construction_nonCopyable();
    void config_defaults();
    void config_setConfig();
    void config_fieldRoundTrip();
    void result_initialDefaults();
    void result_passed_initiallyFalse();
    void result_errors_initiallyZero();
    void result_temperatures_initiallyZero();
    void result_fieldAssignment();
    void computeStressPassed_cleanRun_passes();
    void computeStressPassed_diskErrors_fails();
    void computeStressPassed_memoryErrors_fails();
    void computeStressPassed_gpuErrors_fails();
    void computeStressPassed_abortReason_fails();
    // B5-13: a requested component that could not run must not report PASSED.
    void computeStressPassed_errorsDetected_fails();
    void resolveCpuThreadCount_neverZero();
};

void TestStressTestWorker::construction_default() {
    StressTestWorker worker;
    QVERIFY(worker.parent() == nullptr);
    QVERIFY(!worker.isRunning());
}

void TestStressTestWorker::construction_isWorkerBase() {
    StressTestWorker worker;
    auto* base = qobject_cast<WorkerBase*>(&worker);
    QVERIFY(base != nullptr);
}

void TestStressTestWorker::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<StressTestWorker>);
    QVERIFY(!std::is_move_constructible_v<StressTestWorker>);
}

void TestStressTestWorker::config_defaults() {
    StressTestConfig config;
    QCOMPARE(config.stress_cpu, true);
    QCOMPARE(config.stress_memory, true);
    QCOMPARE(config.stress_disk, false);
    QCOMPARE(config.stress_gpu, false);
    QVERIFY(config.duration_minutes > 0);
    QVERIFY(config.thermal_limit_celsius > 0);
}

void TestStressTestWorker::config_setConfig() {
    StressTestWorker worker;
    StressTestConfig config;
    config.stress_cpu = true;
    config.stress_memory = true;
    config.duration_minutes = 5;
    worker.setConfig(config);
    QVERIFY(!worker.isRunning());
}

void TestStressTestWorker::config_fieldRoundTrip() {
    StressTestConfig config;
    config.stress_cpu = false;
    config.stress_memory = false;
    config.stress_disk = true;
    config.stress_gpu = true;
    config.duration_minutes = 30;
    config.thermal_limit_celsius = 90;

    QVERIFY(!config.stress_cpu);
    QVERIFY(!config.stress_memory);
    QVERIFY(config.stress_disk);
    QVERIFY(config.stress_gpu);
    QCOMPARE(config.duration_minutes, 30);
    QCOMPARE(config.thermal_limit_celsius, 90);
}

void TestStressTestWorker::result_initialDefaults() {
    StressTestWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.duration_seconds, 0);
    QVERIFY(result.abort_reason.isEmpty());
}

void TestStressTestWorker::result_passed_initiallyFalse() {
    StressTestWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.passed, false);
}

void TestStressTestWorker::result_errors_initiallyZero() {
    StressTestWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.errors_detected, 0);
    QCOMPARE(result.memory_pattern_errors, 0);
    QCOMPARE(result.disk_errors, 0);
    QCOMPARE(result.gpu_errors, 0);
}

void TestStressTestWorker::result_temperatures_initiallyZero() {
    StressTestWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.avg_cpu_temp, 0.0);
    QCOMPARE(result.max_cpu_temp, 0.0);
}

void TestStressTestWorker::result_fieldAssignment() {
    StressTestResult result;
    result.passed = true;
    result.duration_seconds = 600;
    result.errors_detected = 0;
    result.memory_pattern_errors = 0;
    result.disk_errors = 0;
    result.gpu_errors = 0;
    result.avg_cpu_temp = 72.5;
    result.max_cpu_temp = 85.0;
    result.abort_reason = QString();

    QVERIFY(result.passed);
    QCOMPARE(result.duration_seconds, 600);
    QCOMPARE(result.errors_detected, 0);
    QCOMPARE(result.avg_cpu_temp, 72.5);
    QCOMPARE(result.max_cpu_temp, 85.0);
    QVERIFY(result.abort_reason.isEmpty());
}

// P07-44: the verdict must fail closed on any per-component error, including disk_errors, which
// are never counted into errors_detected.
void TestStressTestWorker::computeStressPassed_cleanRun_passes() {
    StressTestResult result;
    result.errors_detected = 0;
    result.disk_errors = 0;
    result.memory_pattern_errors = 0;
    result.gpu_errors = 0;
    QVERIFY(StressTestWorker::computeStressPassed(result));
}

void TestStressTestWorker::computeStressPassed_diskErrors_fails() {
    StressTestResult result;
    result.errors_detected = 0;  // disk failures do NOT feed errors_detected
    result.disk_errors = 5;
    QVERIFY(!StressTestWorker::computeStressPassed(result));
}

void TestStressTestWorker::computeStressPassed_memoryErrors_fails() {
    StressTestResult result;
    result.errors_detected = 0;
    result.memory_pattern_errors = 1;
    QVERIFY(!StressTestWorker::computeStressPassed(result));
}

void TestStressTestWorker::computeStressPassed_gpuErrors_fails() {
    StressTestResult result;
    result.errors_detected = 0;
    result.gpu_errors = 1;
    QVERIFY(!StressTestWorker::computeStressPassed(result));
}

void TestStressTestWorker::computeStressPassed_abortReason_fails() {
    StressTestResult result;
    result.errors_detected = 0;
    result.abort_reason = "thermal limit exceeded";
    QVERIFY(!StressTestWorker::computeStressPassed(result));
}

// B5-13: a memory allocation failure folds into errors_detected; the verdict
// must fail closed on it (the memory component never actually ran).
void TestStressTestWorker::computeStressPassed_errorsDetected_fails() {
    StressTestResult result;
    result.errors_detected = 1;
    QVERIFY(!StressTestWorker::computeStressPassed(result));
}

// B5-13: hardware_concurrency() can report 0; a requested CPU stress must still
// launch at least one thread rather than skipping the component and passing.
void TestStressTestWorker::resolveCpuThreadCount_neverZero() {
    // Explicit positive request is honored verbatim.
    QCOMPARE(StressTestWorker::resolveCpuThreadCount(4, 8), 4);
    // "All CPUs" (<=0) uses the detected count when known.
    QCOMPARE(StressTestWorker::resolveCpuThreadCount(0, 8), 8);
    QCOMPARE(StressTestWorker::resolveCpuThreadCount(-1, 8), 8);
    // Detection failure (0) must never yield 0 threads -> clamp to 1.
    QCOMPARE(StressTestWorker::resolveCpuThreadCount(0, 0), 1);
    QCOMPARE(StressTestWorker::resolveCpuThreadCount(-5, 0), 1);
}

QTEST_MAIN(TestStressTestWorker)
#include "test_stress_test_worker.moc"
