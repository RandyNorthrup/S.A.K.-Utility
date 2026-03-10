// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_disk_benchmark_worker.cpp
/// @brief Unit tests for DiskBenchmarkWorker construction, config, result defaults

#include "sak/disk_benchmark_worker.h"

#include <QtTest/QtTest>

using namespace sak;

class TestDiskBenchmarkWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_isWorkerBase();
    void config_defaults();
    void config_setConfig();
    void config_fieldAssignment();
    void result_initialDefaults();
    void result_scores_initiallyZero();
    void result_fieldAssignment();
};

void TestDiskBenchmarkWorker::construction_default() {
    DiskBenchmarkWorker worker;
    QVERIFY(worker.parent() == nullptr);
    QVERIFY(!worker.isRunning());
}

void TestDiskBenchmarkWorker::construction_isWorkerBase() {
    DiskBenchmarkWorker worker;
    auto* base = qobject_cast<WorkerBase*>(&worker);
    QVERIFY(base != nullptr);
}

void TestDiskBenchmarkWorker::config_defaults() {
    DiskBenchmarkConfig config;
    QCOMPARE(config.test_file_size_mb, static_cast<uint64_t>(1024));
    QVERIFY(config.sequential_block_size_kb > 0);
}

void TestDiskBenchmarkWorker::config_setConfig() {
    DiskBenchmarkWorker worker;
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("C:\\");
    config.test_file_size_mb = 256;
    worker.setConfig(config);
    QVERIFY(!worker.isRunning());
}

void TestDiskBenchmarkWorker::config_fieldAssignment() {
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("D:\\");
    config.test_file_size_mb = 512;
    config.sequential_block_size_kb = 2048;

    QCOMPARE(config.drive_path, QStringLiteral("D:\\"));
    QCOMPARE(config.test_file_size_mb, static_cast<uint64_t>(512));
    QCOMPARE(config.sequential_block_size_kb, static_cast<uint64_t>(2048));
}

void TestDiskBenchmarkWorker::result_initialDefaults() {
    DiskBenchmarkWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.seq_read_mbps, 0.0);
    QCOMPARE(result.seq_write_mbps, 0.0);
    QCOMPARE(result.rand_4k_read_mbps, 0.0);
    QCOMPARE(result.rand_4k_write_mbps, 0.0);
}

void TestDiskBenchmarkWorker::result_scores_initiallyZero() {
    DiskBenchmarkWorker worker;
    const auto& result = worker.result();
    QCOMPARE(result.overall_score, 0);
}

void TestDiskBenchmarkWorker::result_fieldAssignment() {
    DiskBenchmarkResult result;
    result.seq_read_mbps = 3500.0;
    result.seq_write_mbps = 3000.0;
    result.rand_4k_read_mbps = 75.0;
    result.rand_4k_write_mbps = 65.0;
    result.overall_score = 95;

    QCOMPARE(result.seq_read_mbps, 3500.0);
    QCOMPARE(result.seq_write_mbps, 3000.0);
    QCOMPARE(result.rand_4k_read_mbps, 75.0);
    QCOMPARE(result.rand_4k_write_mbps, 65.0);
    QCOMPARE(result.overall_score, 95);
}

QTEST_MAIN(TestDiskBenchmarkWorker)
#include "test_disk_benchmark_worker.moc"
