// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_cpu_benchmark_worker.cpp
/// @brief Unit tests for CpuBenchmarkWorker construction, result defaults, signals

#include "sak/cpu_benchmark_worker.h"

#include <QtTest/QtTest>

using namespace sak;

class TestCpuBenchmarkWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_isWorkerBase();
    void result_initialDefaults();
    void result_scores_initiallyZero();
    void result_timestamp_initiallyNull();
};

void TestCpuBenchmarkWorker::construction_default() {
    CpuBenchmarkWorker worker;
    QVERIFY(worker.parent() == nullptr);
    QVERIFY(!worker.isRunning());
}

void TestCpuBenchmarkWorker::construction_isWorkerBase() {
    CpuBenchmarkWorker worker;
    auto* base = qobject_cast<WorkerBase*>(&worker);
    QVERIFY(base != nullptr);
}

void TestCpuBenchmarkWorker::result_initialDefaults() {
    CpuBenchmarkWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.prime_sieve_time_ms, 0.0);
    QCOMPARE(result.matrix_multiply_time_ms, 0.0);
    QCOMPARE(result.zlib_compression_time_ms, 0.0);
    QCOMPARE(result.aes_encryption_time_ms, 0.0);
}

void TestCpuBenchmarkWorker::result_scores_initiallyZero() {
    CpuBenchmarkWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.single_thread_score, 0);
    QCOMPARE(result.multi_thread_score, 0);
}

void TestCpuBenchmarkWorker::result_timestamp_initiallyNull() {
    CpuBenchmarkWorker worker;
    const auto& result = worker.result();
    QVERIFY(!result.timestamp.isValid());
}

QTEST_MAIN(TestCpuBenchmarkWorker)
#include "test_cpu_benchmark_worker.moc"
