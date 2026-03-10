// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_memory_benchmark_worker.cpp
/// @brief Unit tests for MemoryBenchmarkWorker construction and result defaults

#include "sak/memory_benchmark_worker.h"

#include <QtTest/QtTest>

using namespace sak;

class TestMemoryBenchmarkWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_isWorkerBase();
    void result_initialDefaults();
    void result_bandwidths_initiallyZero();
    void result_latency_initiallyZero();
};

void TestMemoryBenchmarkWorker::construction_default() {
    MemoryBenchmarkWorker worker;
    QVERIFY(worker.parent() == nullptr);
    QVERIFY(!worker.isRunning());
}

void TestMemoryBenchmarkWorker::construction_isWorkerBase() {
    MemoryBenchmarkWorker worker;
    auto* base = qobject_cast<WorkerBase*>(&worker);
    QVERIFY(base != nullptr);
}

void TestMemoryBenchmarkWorker::result_initialDefaults() {
    MemoryBenchmarkWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.overall_score, 0);
    QVERIFY(!result.timestamp.isValid());
}

void TestMemoryBenchmarkWorker::result_bandwidths_initiallyZero() {
    MemoryBenchmarkWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.read_bandwidth_gbps, 0.0);
    QCOMPARE(result.write_bandwidth_gbps, 0.0);
    QCOMPARE(result.copy_bandwidth_gbps, 0.0);
}

void TestMemoryBenchmarkWorker::result_latency_initiallyZero() {
    MemoryBenchmarkWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.random_latency_ns, 0.0);
}

QTEST_MAIN(TestMemoryBenchmarkWorker)
#include "test_memory_benchmark_worker.moc"
