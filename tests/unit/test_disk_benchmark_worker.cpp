// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_disk_benchmark_worker.cpp
/// @brief Unit tests for DiskBenchmarkWorker construction, config, result defaults

#include "sak/disk_benchmark_worker.h"

#include <QtTest/QtTest>

#include <type_traits>

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
    // Codex-3 disk:214-228: an absurdly large test-file size must fail closed before
    // total_bytes = size_mb * 1 MiB can wrap size_t.
    void execute_oversizedTestFile_failsClosed();
    void execute_sizeJustBelowMinimum_failsClosed();
    void execute_zeroSequentialPasses_failsClosed();
    void execute_sequentialPassesAboveMax_failsClosed();
    void execute_zeroRandomDuration_failsClosed();
    void execute_outOfRangeQueueDepth_failsClosed();
    // Codex-2 diagnostic_types:254: configured block sizes are wired into the
    // I/O sizing via effectiveBlockBytes -- validate/align, fail closed.
    void effectiveBlockBytes_failsClosedOnNonpositive();
    // Codex-3 disk:398-412: an out-of-range block size is REJECTED (0), not clamped
    // to a bound; inverted bounds (min>max) fail closed instead of invoking clamp UB.
    void effectiveBlockBytes_rejectsOutOfRange();
    void effectiveBlockBytes_acceptsInRangeAndAligns();
};

void TestDiskBenchmarkWorker::construction_default() {
    DiskBenchmarkWorker worker;
    // Default-constructed: the default argument is nullptr, so this documents the
    // default rather than testing forwarding (see the parented case below).
    QVERIFY(worker.parent() == nullptr);
    QVERIFY(!worker.isRunning());

    // DiagnosticController owns its worker via `std::make_unique<DiskBenchmarkWorker>(this)`
    // (diagnostic_controller.cpp:68), so the ctor MUST forward its parent through
    // WorkerBase -> QThread(parent) (disk_benchmark_worker.cpp:316, worker_base.cpp:17).
    // Dropping the argument would leave every production worker outside its owner's
    // object tree -- no destruction-time ownership, no Qt thread-affinity binding.
    // `owner` is declared first so the child unparents itself at scope exit.
    QObject owner;
    DiskBenchmarkWorker parented(&owner);
    QCOMPARE(parented.parent(), &owner);
    QVERIFY(owner.children().contains(&parented));
}

void TestDiskBenchmarkWorker::construction_isWorkerBase() {
    // IS-A is a compile-time fact; the qobject_cast upcast on a stack object can never be
    // null. Assert the inheritance where it can fail (compile time) and pin the moc name,
    // which proves Q_OBJECT is present and correctly namespaced.
    static_assert(std::is_base_of_v<WorkerBase, DiskBenchmarkWorker>,
                  "DiskBenchmarkWorker must inherit WorkerBase.");
    DiskBenchmarkWorker worker;
    QCOMPARE(QByteArray(worker.metaObject()->className()),
             QByteArrayLiteral("sak::DiskBenchmarkWorker"));
}

void TestDiskBenchmarkWorker::config_defaults() {
    DiskBenchmarkConfig config;
    QCOMPARE(config.test_file_size_mb, static_cast<uint64_t>(1024));
    // Default is the compile-time constant kDiskBenchmarkSequentialBlockSizeKb == 1024;
    // > 0 still passes if it silently regressed to any other positive value.
    QCOMPARE(config.sequential_block_size_kb, 1024);
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
    // m_result has no brace-init (disk_benchmark_worker.h:233) and the ctor does not
    // touch it (disk_benchmark_worker.cpp:316), so every zero below comes solely from the
    // NSDMIs at diagnostic_types.h:263-295. Pin ALL of the numeric defaults, not a subset:
    // rand_4k_qd32_*_iops are 60% of calculateScore (disk_benchmark_worker.cpp:1161-1164),
    // and drive_capacity_bytes is left untouched when bytesTotal() <= 0 (:469-472), so on
    // an early-return run (:326/:334/:340, all before the :345 reset) a lost default is
    // handed to the UI as a real capacity.
    QCOMPARE(result.drive_capacity_bytes, static_cast<uint64_t>(0));
    QCOMPARE(result.seq_read_mbps, 0.0);
    QCOMPARE(result.seq_write_mbps, 0.0);
    QCOMPARE(result.rand_4k_read_mbps, 0.0);
    QCOMPARE(result.rand_4k_write_mbps, 0.0);
    QCOMPARE(result.rand_4k_read_iops, 0.0);
    QCOMPARE(result.rand_4k_write_iops, 0.0);
    QCOMPARE(result.rand_4k_qd32_read_mbps, 0.0);
    QCOMPARE(result.rand_4k_qd32_write_mbps, 0.0);
    QCOMPARE(result.rand_4k_qd32_read_iops, 0.0);
    QCOMPARE(result.rand_4k_qd32_write_iops, 0.0);
    QCOMPARE(result.avg_read_latency_us, 0.0);
    QCOMPARE(result.avg_write_latency_us, 0.0);
    QCOMPARE(result.p99_read_latency_us, 0.0);
    QCOMPARE(result.p99_write_latency_us, 0.0);
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

    // The format is "sak_disk_benchmark_<pid>_<msecs>_<counter>.tmp" (C-locale arg, no
    // grouping/padding), so each output is fully deterministic. Pinning the exact strings
    // subsumes uniqueness (a != b != c), the prefix/suffix, and every discriminator.
    QCOMPARE(a, QStringLiteral("sak_disk_benchmark_1234_1000_0.tmp"));
    QCOMPARE(b, QStringLiteral("sak_disk_benchmark_1234_1000_1.tmp"));
    QCOMPARE(c, QStringLiteral("sak_disk_benchmark_1234_2000_0.tmp"));
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

// Codex-3 disk:214-228: test_file_size_mb above the maximum would let
// total_bytes = size_mb * 1 MiB wrap size_t and underflow the offset math. execute()
// must reject it up front with invalid_argument. The drive path is non-empty so
// validation reaches the size check, which returns before any disk I/O happens.
void TestDiskBenchmarkWorker::execute_oversizedTestFile_failsClosed() {
    DiskExecProbe worker;
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("C:\\");
    // Just above the 1 TiB (1024*1024 MB) cap; still well within uint64 so this is a
    // pure validation check, not itself a wrap.
    config.test_file_size_mb = (static_cast<uint64_t>(1024) * 1024) + 1;
    worker.setConfig(config);

    const auto result = worker.runExecute();
    QVERIFY(!result.has_value());
    QCOMPARE(static_cast<int>(result.error()), static_cast<int>(sak::error_code::invalid_argument));
}
// disk_benchmark_worker.cpp:411 is a BOUND (`< kMinTestFileSizeMb`, 16 at :71), not a
// zero-check: 15 MB is refused too. Narrowed to `== 0` it would admit the 1..15 MB files
// the comment at :67-71 says leave too few random-block slots.
void TestDiskBenchmarkWorker::execute_sizeJustBelowMinimum_failsClosed() {
    DiskExecProbe worker;
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("C:\\");
    config.test_file_size_mb = 15;  // kMinTestFileSizeMb (16) - 1
    // Valid but cheap, so a regressed guard fails here instead of benchmarking the disk.
    config.sequential_passes = 1;
    config.random_duration_sec = 1;
    worker.setConfig(config);

    const auto result = worker.runExecute();
    QVERIFY(!result.has_value());
    QCOMPARE(static_cast<int>(result.error()), static_cast<int>(sak::error_code::invalid_argument));
}

// disk_benchmark_worker.cpp:423, low half. 0 passes makes the measurement loops at :713
// and :789 no-ops, leaving best_mbps at its 0.0 init and reporting a 0 MB/s benchmark as
// SUCCESS -- the fail-open the comment at :76-77 says this guard exists to stop.
void TestDiskBenchmarkWorker::execute_zeroSequentialPasses_failsClosed() {
    DiskExecProbe worker;
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("C:\\");
    config.test_file_size_mb = 16;  // kMinTestFileSizeMb: accepted, and cheap
    config.sequential_passes = 0;
    config.random_duration_sec = 1;
    worker.setConfig(config);

    const auto result = worker.runExecute();
    QVERIFY(!result.has_value());
    QCOMPARE(static_cast<int>(result.error()), static_cast<int>(sak::error_code::invalid_argument));
}

// disk_benchmark_worker.cpp:423, high half (kMaxSequentialPasses == 1000 at :78).
void TestDiskBenchmarkWorker::execute_sequentialPassesAboveMax_failsClosed() {
    DiskExecProbe worker;
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("C:\\");
    config.test_file_size_mb = 16;
    config.sequential_passes = 1001;  // kMaxSequentialPasses (1000) + 1
    config.random_duration_sec = 1;
    worker.setConfig(config);

    const auto result = worker.runExecute();
    QVERIFY(!result.has_value());
    QCOMPARE(static_cast<int>(result.error()), static_cast<int>(sak::error_code::invalid_argument));
}

// disk_benchmark_worker.cpp:429: a 0-second random duration measures nothing and still
// reports a figure (kMaxRandomDurationSec == 3600 at :79 caps the * 1000 overflow).
void TestDiskBenchmarkWorker::execute_zeroRandomDuration_failsClosed() {
    DiskExecProbe worker;
    DiskBenchmarkConfig config;
    config.drive_path = QStringLiteral("C:\\");
    config.test_file_size_mb = 16;
    config.sequential_passes = 1;
    config.random_duration_sec = 0;
    worker.setConfig(config);

    const auto result = worker.runExecute();
    QVERIFY(!result.has_value());
    QCOMPARE(static_cast<int>(result.error()), static_cast<int>(sak::error_code::invalid_argument));
}

// disk:444-458: queue_depth_low/high are caller-supplied (setConfig copies the config
// verbatim), not invariants. runRandom4KRead/Write size an aligned buffer as
// block_bytes * queue_depth (disk_benchmark_worker.cpp:878, :1093) and loop queue_depth
// times (:994), so a depth of 0 would issue zero ops per iteration and still report IOPS,
// and an unbounded depth is an unbounded allocation; the Q_ASSERT_X backstop at :867 is
// compiled out in Release. execute() must therefore reject the depth up front.
// Size/passes/duration are all in range here, so invalid_argument can ONLY come from
// validateQueueDepths: if that guard were dropped, execute() would fall through to
// validateDriveAndSpace and report invalid_path for this non-existent volume. The bogus
// volume path also guarantees no benchmark file is ever created, on either path.
void TestDiskBenchmarkWorker::execute_outOfRangeQueueDepth_failsClosed() {
    const QString kUnusableVolume =
        QStringLiteral("\\\\?\\Volume{ffffffff-ffff-ffff-ffff-ffffffffffff}\\");

    // Zero depth: buffer of 0 bytes and a loop that measures nothing.
    {
        DiskExecProbe worker;
        DiskBenchmarkConfig config;
        config.drive_path = kUnusableVolume;
        config.test_file_size_mb = 64;  // inside [16, 1024*1024]
        config.queue_depth_low = 0;
        worker.setConfig(config);

        const auto result = worker.runExecute();
        QVERIFY(!result.has_value());
        QCOMPARE(static_cast<int>(result.error()),
                 static_cast<int>(sak::error_code::invalid_argument));
        // execute() records the target drive (disk_benchmark_worker.cpp:346) only after
        // validation passes, so an empty drive_path proves it stopped at the guard and
        // never entered a benchmark phase.
        QVERIFY(worker.result().drive_path.isEmpty());
    }

    // Above kMaxQueueDepth (1024): an unbounded depth is an unbounded allocation.
    {
        DiskExecProbe worker;
        DiskBenchmarkConfig config;
        config.drive_path = kUnusableVolume;
        config.test_file_size_mb = 64;
        config.queue_depth_high = 1025;
        worker.setConfig(config);

        const auto result = worker.runExecute();
        QVERIFY(!result.has_value());
        QCOMPARE(static_cast<int>(result.error()),
                 static_cast<int>(sak::error_code::invalid_argument));
        QVERIFY(worker.result().drive_path.isEmpty());
    }
}


// A nonpositive configured block size is invalid: effectiveBlockBytes must
// return 0 (fail closed) so execute() aborts rather than running a benchmark
// with a degenerate block size.
void TestDiskBenchmarkWorker::effectiveBlockBytes_failsClosedOnNonpositive() {
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(0, 4, 64), static_cast<size_t>(0));
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(-4, 4, 64), static_cast<size_t>(0));
}

// Codex-3 disk:398-412: an out-of-range positive request is REJECTED (returns 0),
// never silently coerced to the nearest bound -- coercing a wrong input to a default
// is the fallback the standing rule forbids. Inverted/invalid bounds also fail closed
// rather than invoke std::clamp UB (which is undefined when min > max).
void TestDiskBenchmarkWorker::effectiveBlockBytes_rejectsOutOfRange() {
    // Above max and below min both fail closed (previously clamped to a bound).
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(4096, 4, 64), static_cast<size_t>(0));
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(1, 4, 64), static_cast<size_t>(0));
    // The bounds are EXACT, not fuzzy: the first value past each bound is rejected.
    // The sector round-up (:609-612) happens after the range check and must never
    // widen it, so 65 KiB is not "close enough" to the 64 KiB max to be admitted.
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(65, 4, 64), static_cast<size_t>(0));
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(3, 4, 64), static_cast<size_t>(0));
    // Inverted bounds (min > max) and a nonpositive min fail closed, never clamp-UB.
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(8, 64, 4), static_cast<size_t>(0));
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(8, 0, 64), static_cast<size_t>(0));
    // Inverted bounds (min > max) and a nonpositive min fail closed, never clamp-UB.
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(8, 64, 4), static_cast<size_t>(0));
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(8, 0, 64), static_cast<size_t>(0));
}

// In-range values convert KiB->bytes; non-sector-multiple sizes round up to the
// 4096-byte sector boundary so direct I/O (FILE_FLAG_NO_BUFFERING) stays aligned.
void TestDiskBenchmarkWorker::effectiveBlockBytes_acceptsInRangeAndAligns() {
    // Config defaults map to their expected byte sizes.
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(1024, 4, 8192),
             static_cast<size_t>(1024 * 1024));
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(4, 4, 64), static_cast<size_t>(4096));
    // The min and max bounds themselves are accepted (inclusive range).
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(64, 4, 64), static_cast<size_t>(64 * 1024));
    // A non-sector-aligned in-range request (5 KiB = 5120 B) rounds up to 8192.
    QCOMPARE(DiskBenchmarkWorker::effectiveBlockBytes(5, 4, 64), static_cast<size_t>(8192));
}

QTEST_MAIN(TestDiskBenchmarkWorker)
#include "test_disk_benchmark_worker.moc"
