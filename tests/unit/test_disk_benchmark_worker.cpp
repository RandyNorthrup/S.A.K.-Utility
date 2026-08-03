// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_disk_benchmark_worker.cpp
/// @brief Unit tests for DiskBenchmarkWorker construction, config, result defaults

#include "sak/disk_benchmark_worker.h"

#include <QtTest/QtTest>

using namespace sak;

namespace {
// Exposes the protected execute() so a test can drive the validation-only fast
// path without launching the worker thread or touching the disk.
class DiskExecProbe : public DiskBenchmarkWorker {
public:
    std::expected<void, sak::error_code> runExecute() { return execute(); }
};
}  // namespace

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
    // B5-09: unique per-run temp file name (no fixed-name TOCTOU).
    void uniqueBenchmarkFileName_isUniqueAndWellFormed();
    // Codex-2 disk:564: a too-small test-file size must fail closed before any
    // offset arithmetic (max_offset underflows at 0).
    void execute_undersizedTestFile_failsClosed();
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

// B5-09: the benchmark temp file must have a unique per-run name so the fixed
// "sak_disk_benchmark.tmp" TOCTOU (check-then-CREATE_ALWAYS) is gone.
void TestDiskBenchmarkWorker::uniqueBenchmarkFileName_isUniqueAndWellFormed() {
    const QString a = DiskBenchmarkWorker::makeUniqueBenchmarkFileName(1234, 1000, 0);
    const QString b = DiskBenchmarkWorker::makeUniqueBenchmarkFileName(1234, 1000, 1);
    const QString c = DiskBenchmarkWorker::makeUniqueBenchmarkFileName(1234, 2000, 0);

    // A different counter (or timestamp) yields a different name -> no collision.
    QVERIFY(a != b);
    QVERIFY(a != c);
    // Well-formed: keeps the benchmark prefix and the .tmp suffix, and embeds all
    // three discriminators.
    QVERIFY(a.startsWith("sak_disk_benchmark_"));
    QVERIFY(a.endsWith(".tmp"));
    QVERIFY(a.contains("1234"));
    QVERIFY(b.contains("1000"));
    QVERIFY(c.contains("2000"));
}

// Codex-2 disk:564: test_file_size_mb of 0 (or anything below the minimum) would
// underflow max_offset = (file_size / block - 1) * block into a huge value. execute()
// must reject it up front with invalid_argument rather than seek to a garbage offset.
// The drive path is non-empty so validation reaches the size check, and the size check
// returns before any test file is created -- no disk I/O happens.
void TestDiskBenchmarkWorker::execute_undersizedTestFile_failsClosed() {
    DiskExecProbe worker;
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("C:\\");
    config.test_file_size_mb = 0;
    worker.setConfig(config);

    const auto result = worker.runExecute();
    QVERIFY(!result.has_value());
    QCOMPARE(static_cast<int>(result.error()), static_cast<int>(sak::error_code::invalid_argument));
}

QTEST_MAIN(TestDiskBenchmarkWorker)
#include "test_disk_benchmark_worker.moc"
