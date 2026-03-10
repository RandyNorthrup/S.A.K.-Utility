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

QTEST_MAIN(TestStressTestWorker)
#include "test_stress_test_worker.moc"
