// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file disk_benchmark_worker.cpp
/// @brief Disk I/O benchmark implementation using direct I/O

#include "sak/disk_benchmark_worker.h"

#include "sak/action_constants.h"
#include "sak/aligned_buffer.h"
#include "sak/keep_awake.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QStorageInfo>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <vector>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace sak {

namespace {

/// Alignment required for FILE_FLAG_NO_BUFFERING (sector size)
constexpr size_t kSectorAlignment = 4096;
// Sane bounds for the configurable I/O block sizes (KiB). effectiveBlockBytes()
// clamps DiskBenchmarkConfig values into these ranges and fails closed below 1.
constexpr int kSeqBlockMinKb = 4;     // 4 KiB (one sector)
constexpr int kSeqBlockMaxKb = 8192;  // 8 MiB
constexpr int kRandBlockMinKb = 4;    // 4 KiB (one sector)
constexpr int kRandBlockMaxKb = 64;   // 64 KiB
constexpr int kDiskBenchmarkStepTotal = 8;
constexpr int kDiskProgressSequentialRead = 1;
constexpr int kDiskProgressSequentialWrite = 2;
constexpr int kDiskProgressRandomQd1Read = 3;
constexpr int kDiskProgressRandomQd1Write = 4;
constexpr int kDiskProgressRandomQd32Read = 5;
constexpr int kDiskProgressRandomQd32Write = 6;
constexpr int kDiskProgressScore = 7;
constexpr unsigned int kRandomWriteFillSeed = 999;
constexpr unsigned int kCreateFileSeed = 789;
constexpr unsigned int kSequentialWriteSeed = 321;
constexpr unsigned int kRandomReadOffsetSeed = 654;
constexpr unsigned int kRandomWriteOffsetSeed = 876;
constexpr double kNanosecondsPerSecond = 1'000'000'000.0;
constexpr double kNanosecondsPerMicrosecond = 1000.0;
constexpr int kBenchmarkMillisecondsPerSecond = 1000;
constexpr size_t kLatencySampleReserve = 100'000;
constexpr double kP99Percentile = 0.99;
// Smallest sane test-file size. Below this the random-I/O offset math
// (file_size / random_block - 1) underflows to a huge value at 0, and there are
// too few block slots for a meaningful random benchmark. The 16 MiB floor also
// stays well above the 64 KiB max random block so the slot count is never < 1.
constexpr uint64_t kMinTestFileSizeMb = 16;
// Upper bound on the test-file size. Without a maximum, total_bytes =
// test_file_size_mb * 1 MiB could (for absurd ~2^44 MB inputs) wrap size_t and
// underflow the offset math; cap at 1 TiB, far above any real benchmark file.
constexpr uint64_t kMaxTestFileSizeMb = 1024ULL * 1024;  // 1 TiB
// A non-positive pass count would skip the measurement loop entirely and report a
// bogus 0 MB/s "success"; an unbounded duration would overflow duration_sec * 1000.
constexpr int kMaxSequentialPasses = 1000;
constexpr int kMaxRandomDurationSec = 3600;  // 1 hour; * 1000 stays within int range
constexpr double kSeqReadScoreWeight = 0.20;
constexpr double kSeqWriteScoreWeight = 0.20;
constexpr double kRandReadScoreWeight = 0.30;
constexpr double kRandWriteScoreWeight = 0.30;
constexpr double kDiskScoreScale = 1000.0;

/// Reference scores: Samsung 980 PRO 1TB NVMe = 1000
constexpr double kRefSeqReadMbps = 7000.0;
constexpr double kRefSeqWriteMbps = 5000.0;
constexpr double kRefRand4KReadIops = 800000.0;
constexpr double kRefRand4KWriteIops = 1000000.0;

#ifdef SAK_PLATFORM_WINDOWS
// Fail closed if a freshly-opened handle resolved to a reparse point (a symlink or
// junction planted at the guessable temp path between subtests) or to a file with
// more than one hard link (a same-name hard-link swap onto an unrelated target).
// createTestFile() claimed a unique regular file via CREATE_NEW, so our file always
// has exactly one link and no reparse attribute. Closes @p h and returns
// INVALID_HANDLE_VALUE on any mismatch or info-query failure.
HANDLE rejectReparseOrLinkedFile(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || info.nNumberOfLinks != 1) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

// All benchmark reopens of the claimed temp file open the NAME itself with
// FILE_FLAG_OPEN_REPARSE_POINT and then verify (via rejectReparseOrLinkedFile) that
// it is still our unique regular file, never following a symlink/junction/hard-link
// swapped in at the guessable path between subtests.
HANDLE openRandomWriteFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH |
                               FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    return rejectReparseOrLinkedFile(h);
}

HANDLE openRandomReadFile(const std::wstring& path) {
    HANDLE h =
        CreateFileW(path.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
    return rejectReparseOrLinkedFile(h);
}

HANDLE openSequentialReadFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN |
                               FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    return rejectReparseOrLinkedFile(h);
}

// Reopen the already-claimed benchmark file for overwriting WITHOUT traversing a
// reparse point. createTestFile() exclusively claimed a unique file via CREATE_NEW;
// reopening the guessable path with CREATE_ALWAYS would follow a symlink or junction
// swapped in between subtests and truncate/create an unrelated target.
HANDLE openBenchmarkFileForOverwrite(const std::wstring& path) {
    HANDLE h =
        CreateFileW(path.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
    return rejectReparseOrLinkedFile(h);
}

void fillRandomWriteBuffer(AlignedBuffer& buffer) {
    std::mt19937 fill_rng(kRandomWriteFillSeed);
    auto* data32 = reinterpret_cast<uint32_t*>(buffer.data());
    for (size_t i = 0; i < buffer.size() / sizeof(uint32_t); ++i) {
        data32[i] = fill_rng();
    }
}

void setRandomWriteResults(uint64_t total_ops,
                           uint64_t total_bytes,
                           double elapsed_sec,
                           double& write_mbps,
                           double& iops) {
    iops = static_cast<double>(total_ops) / elapsed_sec;
    write_mbps = static_cast<double>(total_bytes) / sak::kBytesPerMBf / elapsed_sec;
}

// A random-I/O phase is only scoreable if at least one op completed AND completed
// ops outnumber failures. Zero completed ops means every op in the window failed;
// more failures than successes means a mostly-broken drive whose IOPS would be
// garbage. Either way, fail closed rather than report a misleading number.
bool randomIoResultUsable(uint64_t total_ops, uint64_t total_failures) {
    // Completed ops must strictly OUTNUMBER failures: failures == completed (a 50%
    // failure rate) is a mostly-broken drive whose IOPS would be garbage, so reject it.
    return total_ops > 0 && total_failures < total_ops;
}

// Compute the average latency and optionally hand the raw samples to the caller. Shared by the
// random read and write subtests.
void finalizeLatencies(std::vector<double>& latencies,
                       double& avg_latency_us,
                       std::vector<double>* latencies_out) {
    if (!latencies.empty()) {
        avg_latency_us = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                         static_cast<double>(latencies.size());
    }
    if (latencies_out) {
        *latencies_out = std::move(latencies);
    }
}

// Fill a freshly-created benchmark file with @p total_bytes of reproducible
// pseudo-random data via direct, sector-aligned writes of @p block_bytes. Closes
// @p h on failure.
bool fillTestFileWithRandom(HANDLE h, size_t total_bytes, size_t block_bytes) {
    AlignedBuffer buf(block_bytes, kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return false;
    }

    std::mt19937 rng(kCreateFileSeed);
    auto* data32 = reinterpret_cast<uint32_t*>(buf.data());
    const size_t count32 = buf.size() / sizeof(uint32_t);
    for (size_t i = 0; i < count32; ++i) {
        data32[i] = rng();
    }

    size_t written_total = 0;
    while (written_total < total_bytes) {
        DWORD bytes_written = 0;
        const DWORD to_write =
            static_cast<DWORD>(std::min(buf.size(), total_bytes - written_total));
        if (!WriteFile(h, buf.data(), to_write, &bytes_written, nullptr)) {
            logError("Failed to write test file at offset {}", written_total);
            CloseHandle(h);
            return false;
        }
        written_total += bytes_written;
    }
    return true;
}
#endif

}  // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

DiskBenchmarkWorker::DiskBenchmarkWorker(QObject* parent) : WorkerBase(parent) {}

// ============================================================================
// WorkerBase Override
// ============================================================================

auto DiskBenchmarkWorker::execute() -> std::expected<void, sak::error_code> {
    sak::KeepAwakeGuard keep_awake(sak::KeepAwake::PowerRequest::System, "Disk benchmark");

    // Validate drive path before starting
    if (m_config.drive_path.isEmpty()) {
        logError("Disk benchmark: no drive path specified");
        return std::unexpected(sak::error_code::invalid_path);
    }

    // Fail closed on a test-file size outside the sane range. Below the minimum the
    // random-I/O offset math (file_size / block - 1) underflows to a huge value at 0;
    // above the maximum total_bytes could wrap size_t. Validate before any arithmetic.
    if (auto status = validateTestFileSize(); !status) {
        return status;
    }

    // Wire the configured block sizes into the actual I/O; fail closed on a
    // nonpositive size before any benchmark phase runs.
    if (auto status = resolveBlockSizes(); !status) {
        return status;
    }

    logInfo("Starting disk benchmark on {}", m_config.drive_path.toStdString());
    m_result = DiskBenchmarkResult{};
    m_result.drive_path = m_config.drive_path;

    // Resolve drive info and fail closed on an unusable target or insufficient space.
    if (auto status = validateDriveAndSpace(); !status) {
        return status;
    }

    // Create test file
    reportProgress(sak::progress::kStart, kDiskBenchmarkStepTotal, "Creating test file...");
    if (!createTestFile()) {
        // A fill failure (e.g. disk full mid-write) leaves a partially-written temp
        // file after m_test_file_path was recorded; remove it rather than orphan it.
        cleanupTestFile();
        return std::unexpected(sak::error_code::read_error);
    }

    auto benchmark_result = runAllBenchmarks();
    if (!benchmark_result) {
        // A subtest that failed I/O (not just cancellation) leaves the test file behind; remove
        // it so the error path does not leak the temp file. cleanupTestFile is idempotent.
        cleanupTestFile();
        return benchmark_result;
    }

    // A cancel during the FINAL subtest only breaks its timed loop and still returns
    // success; catch it here so a truncated run is not scored/reported as complete.
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }

    // Cleanup and score
    reportProgress(kDiskProgressScore, kDiskBenchmarkStepTotal, "Calculating scores...");
    cleanupTestFile();
    calculateScore();

    m_result.timestamp = QDateTime::currentDateTime();

    logInfo("Disk benchmark complete -- Seq R/W: {:.0f}/{:.0f} MB/s, 4K R/W: {:.0f}/{:.0f} IOPS",
            m_result.seq_read_mbps,
            m_result.seq_write_mbps,
            m_result.rand_4k_read_iops,
            m_result.rand_4k_write_iops);

    Q_EMIT benchmarkComplete(m_result);
    return {};
}

auto DiskBenchmarkWorker::resolveBlockSizes() -> std::expected<void, sak::error_code> {
    // effectiveBlockBytes clamps to a sane range and rounds to the sector
    // boundary; a nonpositive configured size returns 0 and we fail closed rather
    // than run a benchmark with a degenerate block size.
    m_seq_block_bytes =
        effectiveBlockBytes(m_config.sequential_block_size_kb, kSeqBlockMinKb, kSeqBlockMaxKb);
    m_rand_block_bytes =
        effectiveBlockBytes(m_config.random_block_size_kb, kRandBlockMinKb, kRandBlockMaxKb);
    if (m_seq_block_bytes == 0 || m_rand_block_bytes == 0) {
        logError("Disk benchmark: nonpositive block size (seq {} KB, rand {} KB)",
                 m_config.sequential_block_size_kb,
                 m_config.random_block_size_kb);
        return std::unexpected(sak::error_code::invalid_argument);
    }
    return {};
}

auto DiskBenchmarkWorker::validateTestFileSize() const -> std::expected<void, sak::error_code> {
    if (m_config.test_file_size_mb < kMinTestFileSizeMb) {
        logError("Disk benchmark: test_file_size_mb {} below minimum {} MB",
                 m_config.test_file_size_mb,
                 kMinTestFileSizeMb);
        return std::unexpected(sak::error_code::invalid_argument);
    }
    if (m_config.test_file_size_mb > kMaxTestFileSizeMb) {
        logError("Disk benchmark: test_file_size_mb {} above maximum {} MB",
                 m_config.test_file_size_mb,
                 kMaxTestFileSizeMb);
        return std::unexpected(sak::error_code::invalid_argument);
    }
    if (m_config.sequential_passes <= 0 || m_config.sequential_passes > kMaxSequentialPasses) {
        logError("Disk benchmark: sequential_passes {} out of range (1..{})",
                 m_config.sequential_passes,
                 kMaxSequentialPasses);
        return std::unexpected(sak::error_code::invalid_argument);
    }
    if (m_config.random_duration_sec <= 0 || m_config.random_duration_sec > kMaxRandomDurationSec) {
        logError("Disk benchmark: random_duration_sec {} out of range (1..{})",
                 m_config.random_duration_sec,
                 kMaxRandomDurationSec);
        return std::unexpected(sak::error_code::invalid_argument);
    }
    return {};
}

auto DiskBenchmarkWorker::validateDriveAndSpace() -> std::expected<void, sak::error_code> {
    QStorageInfo storage(m_config.drive_path);
    if (!storage.isValid() || !storage.isReady()) {
        logError("Disk benchmark: drive path not valid/ready: {}",
                 m_config.drive_path.toStdString());
        return std::unexpected(sak::error_code::invalid_path);
    }
    // bytesTotal() is signed and returns -1 on error; only record a real capacity so
    // a negative value is never cast into a huge uint64 display figure.
    const qint64 total = storage.bytesTotal();
    if (total > 0) {
        m_result.drive_capacity_bytes = static_cast<uint64_t>(total);
    }
    const uint64_t required = m_config.test_file_size_mb * static_cast<uint64_t>(sak::kBytesPerMB);
    const qint64 available = storage.bytesAvailable();
    if (available < 0 || static_cast<uint64_t>(available) < required) {
        logError("Disk benchmark: insufficient free space ({} bytes available, {} required)",
                 available,
                 required);
        return std::unexpected(sak::error_code::insufficient_disk_space);
    }
    return {};
}

auto DiskBenchmarkWorker::continueOrCancelBenchmark() -> std::expected<void, sak::error_code> {
    if (checkStop()) {
        cleanupTestFile();
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    return {};
}

auto DiskBenchmarkWorker::runSequentialBenchmarks() -> std::expected<void, sak::error_code> {
    reportProgress(kDiskProgressSequentialRead,
                   kDiskBenchmarkStepTotal,
                   "Sequential read benchmark...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    if (auto status = runSequentialRead(); !status) {
        return status;
    }

    reportProgress(kDiskProgressSequentialWrite,
                   kDiskBenchmarkStepTotal,
                   "Sequential write benchmark...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    return runSequentialWrite();
}

auto DiskBenchmarkWorker::runRandomQd1Benchmarks() -> std::expected<void, sak::error_code> {
    reportProgress(kDiskProgressRandomQd1Read, kDiskBenchmarkStepTotal, "Random 4K QD1 read...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    if (auto status = runRandom4KRead(m_config.queue_depth_low,
                                      m_result.rand_4k_read_mbps,
                                      m_result.rand_4k_read_iops,
                                      m_result.avg_read_latency_us);
        !status) {
        return status;
    }

    reportProgress(kDiskProgressRandomQd1Write, kDiskBenchmarkStepTotal, "Random 4K QD1 write...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    return runRandom4KWrite(m_config.queue_depth_low,
                            m_result.rand_4k_write_mbps,
                            m_result.rand_4k_write_iops,
                            m_result.avg_write_latency_us);
}

auto DiskBenchmarkWorker::runRandomQd32Benchmarks() -> std::expected<void, sak::error_code> {
    std::vector<double> qd32_read_latencies;
    std::vector<double> qd32_write_latencies;
    double qd32_avg_read_lat = 0.0;
    double qd32_avg_write_lat = 0.0;

    reportProgress(kDiskProgressRandomQd32Read, kDiskBenchmarkStepTotal, "Random 4K QD32 read...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    if (auto status = runRandom4KRead(m_config.queue_depth_high,
                                      m_result.rand_4k_qd32_read_mbps,
                                      m_result.rand_4k_qd32_read_iops,
                                      qd32_avg_read_lat,
                                      &qd32_read_latencies);
        !status) {
        return status;
    }
    m_result.p99_read_latency_us = calculateP99(qd32_read_latencies);

    reportProgress(kDiskProgressRandomQd32Write,
                   kDiskBenchmarkStepTotal,
                   "Random 4K QD32 write...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    if (auto status = runRandom4KWrite(m_config.queue_depth_high,
                                       m_result.rand_4k_qd32_write_mbps,
                                       m_result.rand_4k_qd32_write_iops,
                                       qd32_avg_write_lat,
                                       &qd32_write_latencies);
        !status) {
        return status;
    }
    m_result.p99_write_latency_us = calculateP99(qd32_write_latencies);

    return {};
}

auto DiskBenchmarkWorker::runAllBenchmarks() -> std::expected<void, sak::error_code> {
    if (auto status = runSequentialBenchmarks(); !status) {
        return status;
    }
    if (auto status = runRandomQd1Benchmarks(); !status) {
        return status;
    }
    return runRandomQd32Benchmarks();
}

// ============================================================================
// Test File Management
// ============================================================================

QString DiskBenchmarkWorker::makeUniqueBenchmarkFileName(quint32 pid,
                                                         qint64 msecs,
                                                         quint64 counter) {
    return QStringLiteral("sak_disk_benchmark_%1_%2_%3.tmp").arg(pid).arg(msecs).arg(counter);
}

size_t DiskBenchmarkWorker::effectiveBlockBytes(int block_kb, int min_kb, int max_kb) {
    // Fail closed on invalid bounds (min_kb <= 0 or max_kb < min_kb) -- std::clamp is
    // UB when min > max -- and on an out-of-range request. An out-of-range block size
    // is REJECTED, not silently coerced to a bound: coercing a wrong input to a
    // "nearest valid" default is exactly the fallback the standing rule forbids. The
    // caller treats 0 as an error and aborts the benchmark.
    if (min_kb <= 0 || max_kb < min_kb) {
        return 0;
    }
    if (block_kb < min_kb || block_kb > max_kb) {
        return 0;
    }
    size_t bytes = static_cast<size_t>(block_kb) * static_cast<size_t>(sak::kBytesPerKB);
    // Direct I/O (FILE_FLAG_NO_BUFFERING) requires sector-aligned transfers; round
    // up to the next sector so every read/write stays aligned.
    const size_t remainder = bytes % kSectorAlignment;
    if (remainder != 0) {
        bytes += kSectorAlignment - remainder;
    }
    return bytes;
}

QString DiskBenchmarkWorker::testFilePath() const {
    return m_test_file_path;
}

bool DiskBenchmarkWorker::createTestFile() {
#ifdef SAK_PLATFORM_WINDOWS
    // Claim a UNIQUE per-run file with an exclusive CREATE_NEW: this removes the
    // old fixed-name TOCTOU (QFile::exists() check then CREATE_ALWAYS), where a
    // file or symlink planted at the guessable path between the check and the
    // create would be truncated here and deleted by cleanupTestFile(). CREATE_NEW
    // fails closed if anything already exists at the name, and the unique name
    // also keeps concurrent runs from colliding.
    static std::atomic<quint64> s_counter{0};
    const quint64 seq = s_counter.fetch_add(1, std::memory_order_relaxed);
    const QString name = makeUniqueBenchmarkFileName(GetCurrentProcessId(),
                                                     QDateTime::currentMSecsSinceEpoch(),
                                                     seq);
    const QString path = QDir(m_config.drive_path).filePath(name);
    const std::wstring wpath = path.toStdWString();

    // Create exclusively with direct I/O flags -- one call is both the atomic
    // claim (CREATE_NEW) and the open, so there is no check-then-open window.
    HANDLE h =
        CreateFileW(wpath.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_NEW,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Disk benchmark: failed to exclusively create test file: {}", path.toStdString());
        return false;
    }
    // Record the claimed path only after a successful exclusive create, so every
    // read/write/cleanup targets exactly the file we own.
    m_test_file_path = path;

    const size_t total_bytes = static_cast<size_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    if (!fillTestFileWithRandom(h, total_bytes, m_seq_block_bytes)) {
        return false;  // helper closed the handle
    }

    CloseHandle(h);
    logInfo("Test file created: {} ({} MB)", path.toStdString(), m_config.test_file_size_mb);
    return true;
#else
    return false;
#endif
}

void DiskBenchmarkWorker::cleanupTestFile() {
    const QString path = testFilePath();
    if (path.isEmpty()) {
        return;  // nothing was ever claimed this run
    }
    if (QFile::exists(path)) {
        if (QFile::remove(path)) {
            logInfo("Test file removed: {}", path.toStdString());
        } else {
            // Do not claim success we did not achieve: a locked/permission-denied
            // delete leaves the temp file behind and the caller/log must reflect that.
            logError("Test file could not be removed (still present): {}", path.toStdString());
        }
    }
    m_test_file_path.clear();  // do not delete the same path twice
}

// ============================================================================
// Sequential Benchmarks
// ============================================================================

auto DiskBenchmarkWorker::runSequentialRead() -> std::expected<void, sak::error_code> {
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    // Open the claimed name itself and fail closed if it is now a reparse point or
    // hard-linked target swapped in between subtests. See openSequentialReadFile.
    HANDLE h = openSequentialReadFile(wpath);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Sequential read: Failed to open test file");
        return std::unexpected(sak::error_code::read_error);
    }

    AlignedBuffer buf(m_seq_block_bytes, kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return std::unexpected(sak::error_code::out_of_memory);
    }

    const size_t total_bytes = static_cast<size_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    double best_mbps = 0.0;

    for (int pass = 0; pass < m_config.sequential_passes; ++pass) {
        QElapsedTimer timer;
        timer.start();

        size_t bytes_read_total = readSequentialPass(h, buf.data(), buf.size(), total_bytes);

        // A short read means ReadFile failed or hit an early EOF mid-run: a genuine
        // disk error, not a measurement. Fail closed rather than scoring the partial
        // transfer as throughput.
        if (bytes_read_total < total_bytes) {
            CloseHandle(h);
            logError("Sequential read: short read ({} of {} bytes) -- disk I/O error",
                     bytes_read_total,
                     total_bytes);
            return std::unexpected(sak::error_code::read_error);
        }

        const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
        const double mbps = static_cast<double>(bytes_read_total) / sak::kBytesPerMBf / elapsed_sec;
        best_mbps = std::max(best_mbps, mbps);
    }

    CloseHandle(h);
    m_result.seq_read_mbps = best_mbps;
    logInfo("Sequential read: {:.0f} MB/s", best_mbps);
    return {};
#else
    return {};
#endif
}

#ifdef SAK_PLATFORM_WINDOWS
size_t DiskBenchmarkWorker::readSequentialPass(void* file_handle,
                                               uint8_t* buffer,
                                               size_t bufSize,
                                               size_t total_bytes) {
    HANDLE h = static_cast<HANDLE>(file_handle);
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) {
        return 0;
    }

    size_t bytes_read_total = 0;
    while (bytes_read_total < total_bytes) {
        DWORD bytes_read = 0;
        if (!ReadFile(h, buffer, static_cast<DWORD>(bufSize), &bytes_read, nullptr) ||
            bytes_read == 0) {
            break;
        }
        bytes_read_total += bytes_read;
    }
    return bytes_read_total;
}
#endif

auto DiskBenchmarkWorker::runSequentialWrite() -> std::expected<void, sak::error_code> {
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    AlignedBuffer buf(m_seq_block_bytes, kSectorAlignment);
    if (!buf.valid()) {
        return std::unexpected(sak::error_code::out_of_memory);
    }

    // Fill buffer with data
    std::mt19937 rng(kSequentialWriteSeed);
    auto* data32 = reinterpret_cast<uint32_t*>(buf.data());
    const size_t count32 = buf.size() / sizeof(uint32_t);
    for (size_t i = 0; i < count32; ++i) {
        data32[i] = rng();
    }

    const size_t total_bytes = static_cast<size_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    double best_mbps = 0.0;

    for (int pass = 0; pass < m_config.sequential_passes; ++pass) {
        // Overwrite the file createTestFile() already claimed (which is full-size)
        // rather than re-creating the guessable path with CREATE_ALWAYS, which would
        // follow a reparse point swapped in between subtests. See
        // openBenchmarkFileForOverwrite.
        HANDLE h = openBenchmarkFileForOverwrite(wpath);

        if (h == INVALID_HANDLE_VALUE) {
            logError("Sequential write: Failed to open test file");
            return std::unexpected(sak::error_code::write_error);
        }

        QElapsedTimer timer;
        timer.start();

        size_t written_total = writeSequentialPass(h, buf.data(), buf.size(), total_bytes);

        FlushFileBuffers(h);
        CloseHandle(h);

        // A short write means WriteFile failed mid-run: a genuine disk error, not a
        // measurement. Fail closed rather than scoring the partial transfer.
        if (written_total < total_bytes) {
            logError("Sequential write: short write ({} of {} bytes) -- disk I/O error",
                     written_total,
                     total_bytes);
            return std::unexpected(sak::error_code::write_error);
        }

        const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
        const double mbps = static_cast<double>(written_total) / sak::kBytesPerMBf / elapsed_sec;
        best_mbps = std::max(best_mbps, mbps);
    }

    m_result.seq_write_mbps = best_mbps;
    logInfo("Sequential write: {:.0f} MB/s", best_mbps);
    return {};
#else
    return {};
#endif
}

#ifdef SAK_PLATFORM_WINDOWS
size_t DiskBenchmarkWorker::writeSequentialPass(void* file_handle,
                                                const uint8_t* buffer,
                                                size_t bufSize,
                                                size_t total_bytes) {
    HANDLE h = static_cast<HANDLE>(file_handle);
    size_t written_total = 0;
    while (written_total < total_bytes) {
        DWORD bytes_written = 0;
        const DWORD to_write = static_cast<DWORD>(std::min(bufSize, total_bytes - written_total));
        if (!WriteFile(h, buffer, to_write, &bytes_written, nullptr)) {
            break;
        }
        written_total += bytes_written;
    }
    return written_total;
}
#endif

// ============================================================================
// Random I/O Benchmarks
// ============================================================================

auto DiskBenchmarkWorker::runRandom4KRead(int queue_depth,
                                          double& read_mbps,
                                          double& iops,
                                          double& avg_latency_us,
                                          std::vector<double>* latencies_out)
    -> std::expected<void, sak::error_code> {
    Q_ASSERT_X(queue_depth > 0, "runRandom4KRead", "queue_depth must be positive");
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    HANDLE h = openRandomReadFile(wpath);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Random read: Failed to open test file");
        return std::unexpected(sak::error_code::read_error);
    }

    AlignedBuffer buf(m_rand_block_bytes * static_cast<size_t>(queue_depth), kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return std::unexpected(sak::error_code::out_of_memory);
    }

    const uint64_t file_size = static_cast<uint64_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    const uint64_t max_offset = (file_size / m_rand_block_bytes - 1) * m_rand_block_bytes;
    const int duration_ms = m_config.random_duration_sec * kBenchmarkMillisecondsPerSecond;

    std::vector<double> latencies;
    latencies.reserve(kLatencySampleReserve);
    uint64_t total_ops = 0;
    uint64_t total_bytes = 0;
    uint64_t total_failures = 0;
    RandomIoStats stats{latencies, total_ops, total_bytes, total_failures};
    const RandomIoLoopConfig loop_config{queue_depth, max_offset, duration_ms};

    const double elapsed_sec = runRandom4KReadLoop(h, buf.data(), loop_config, stats);

    CloseHandle(h);

    if (!randomIoResultUsable(total_ops, total_failures)) {
        logError("Random read: {} ops completed, {} failed -- disk error",
                 total_ops,
                 total_failures);
        return std::unexpected(sak::error_code::read_error);
    }

    iops = static_cast<double>(total_ops) / elapsed_sec;
    read_mbps = static_cast<double>(total_bytes) / sak::kBytesPerMBf / elapsed_sec;

    finalizeLatencies(latencies, avg_latency_us, latencies_out);

    logInfo("Random {}K read QD{}: {:.0f} IOPS, {:.1f} MB/s, {:.0f} us avg",
            m_rand_block_bytes / static_cast<size_t>(sak::kBytesPerKB),
            queue_depth,
            iops,
            read_mbps,
            avg_latency_us);
    return {};
#else
    logWarning("Random 4K read benchmark requires Windows platform");
    (void)queue_depth;
    (void)read_mbps;
    (void)iops;
    (void)avg_latency_us;
    (void)latencies_out;
    return {};
#endif
}

#ifdef SAK_PLATFORM_WINDOWS
void DiskBenchmarkWorker::processRandomReadOp(
    void* file_handle, uint8_t* buf_data, int queue_index, uint64_t offset, RandomIoStats& stats) {
    HANDLE h = static_cast<HANDLE>(file_handle);

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        logWarning("Random read: SetFilePointerEx failed at offset {} (error {})",
                   offset,
                   GetLastError());
        ++stats.total_failures;
        return;
    }

    QElapsedTimer op_timer;
    op_timer.start();

    DWORD bytes_read = 0;
    // A short read (bytes_read < block) with NO_BUFFERING and an in-bounds aligned
    // offset is a real disk error, not a completed op -- count it as a failure so one
    // success cannot mask many partial/failed reads and report garbage IOPS.
    if (!ReadFile(h,
                  buf_data + queue_index * m_rand_block_bytes,
                  static_cast<DWORD>(m_rand_block_bytes),
                  &bytes_read,
                  nullptr) ||
        bytes_read != static_cast<DWORD>(m_rand_block_bytes)) {
        logWarning("Random read: short/failed ReadFile at offset {} (error {})",
                   offset,
                   GetLastError());
        ++stats.total_failures;
        return;
    }

    stats.latencies.push_back(op_timer.nsecsElapsed() / kNanosecondsPerMicrosecond);
    stats.total_bytes += bytes_read;
    ++stats.total_ops;
}
#endif

#ifdef SAK_PLATFORM_WINDOWS
double DiskBenchmarkWorker::runRandom4KReadLoop(void* file_handle,
                                                uint8_t* buf_data,
                                                const RandomIoLoopConfig& config,
                                                RandomIoStats& stats) {
    std::mt19937_64 rng(kRandomReadOffsetSeed);
    std::uniform_int_distribution<uint64_t> offset_dist(0, config.max_offset / m_rand_block_bytes);

    QElapsedTimer total_timer;
    total_timer.start();

    // NOTE: each op is issued and completed synchronously on one non-overlapped
    // handle, so a queue_depth > 1 measures serialized (effectively QD1) latency, not
    // true concurrent queue depth. This is a known benchmark-fidelity limitation, not
    // a correctness/data bug; true QD would require overlapped/async I/O.
    while (total_timer.elapsed() < config.duration_ms) {
        if (stopRequested()) {
            break;
        }

        for (int queue_index = 0; queue_index < config.queue_depth; ++queue_index) {
            const uint64_t offset = offset_dist(rng) * m_rand_block_bytes;
            processRandomReadOp(file_handle, buf_data, queue_index, offset, stats);
        }
    }

    return total_timer.nsecsElapsed() / kNanosecondsPerSecond;
}
#endif

#ifdef SAK_PLATFORM_WINDOWS
void DiskBenchmarkWorker::processRandomWriteOp(void* file_handle,
                                               const uint8_t* buf_data,
                                               int queue_index,
                                               uint64_t offset,
                                               RandomIoStats& stats) {
    HANDLE h = static_cast<HANDLE>(file_handle);

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        logWarning("Random write: SetFilePointerEx failed at offset {} (error {})",
                   offset,
                   GetLastError());
        ++stats.total_failures;
        return;
    }

    QElapsedTimer op_timer;
    op_timer.start();

    DWORD bytes_written = 0;
    // A short write (bytes_written < block) with NO_BUFFERING and an in-bounds aligned
    // offset is a real disk error, not a completed op -- count it as a failure so one
    // success cannot mask many partial/failed writes and report garbage IOPS.
    if (!WriteFile(h,
                   buf_data + queue_index * m_rand_block_bytes,
                   static_cast<DWORD>(m_rand_block_bytes),
                   &bytes_written,
                   nullptr) ||
        bytes_written != static_cast<DWORD>(m_rand_block_bytes)) {
        logWarning("Random write: short/failed WriteFile at offset {} (error {})",
                   offset,
                   GetLastError());
        ++stats.total_failures;
        return;
    }

    stats.latencies.push_back(op_timer.nsecsElapsed() / kNanosecondsPerMicrosecond);
    stats.total_bytes += bytes_written;
    ++stats.total_ops;
}
#endif

#ifdef SAK_PLATFORM_WINDOWS
double DiskBenchmarkWorker::runRandom4KWriteLoop(void* file_handle,
                                                 const uint8_t* buf_data,
                                                 const RandomIoLoopConfig& config,
                                                 RandomIoStats& stats) {
    std::mt19937_64 rng(kRandomWriteOffsetSeed);
    std::uniform_int_distribution<uint64_t> offset_dist(0, config.max_offset / m_rand_block_bytes);

    QElapsedTimer total_timer;
    total_timer.start();

    while (total_timer.elapsed() < config.duration_ms) {
        if (stopRequested()) {
            break;
        }

        for (int queue_index = 0; queue_index < config.queue_depth; ++queue_index) {
            const uint64_t offset = offset_dist(rng) * m_rand_block_bytes;
            processRandomWriteOp(file_handle, buf_data, queue_index, offset, stats);
        }
    }

    return total_timer.nsecsElapsed() / kNanosecondsPerSecond;
}
#endif

auto DiskBenchmarkWorker::runRandom4KWrite(int queue_depth,
                                           double& write_mbps,
                                           double& iops,
                                           double& avg_latency_us,
                                           std::vector<double>* latencies_out)
    -> std::expected<void, sak::error_code> {
    Q_ASSERT_X(queue_depth > 0, "runRandom4KWrite", "queue_depth must be positive");
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    HANDLE h = openRandomWriteFile(wpath);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Random write: Failed to open test file");
        return std::unexpected(sak::error_code::write_error);
    }

    AlignedBuffer buf(m_rand_block_bytes * static_cast<size_t>(queue_depth), kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return std::unexpected(sak::error_code::out_of_memory);
    }

    fillRandomWriteBuffer(buf);

    const uint64_t file_size = static_cast<uint64_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    const uint64_t max_offset = (file_size / m_rand_block_bytes - 1) * m_rand_block_bytes;

    std::vector<double> latencies;
    latencies.reserve(kLatencySampleReserve);
    uint64_t total_ops = 0;
    uint64_t total_bytes = 0;
    uint64_t total_failures = 0;
    RandomIoStats stats{latencies, total_ops, total_bytes, total_failures};
    const RandomIoLoopConfig loop_config{
        queue_depth, max_offset, m_config.random_duration_sec * kBenchmarkMillisecondsPerSecond};

    const double elapsed_sec = runRandom4KWriteLoop(h, buf.data(), loop_config, stats);

    CloseHandle(h);

    if (!randomIoResultUsable(total_ops, total_failures)) {
        logError("Random write: {} ops completed, {} failed -- disk error",
                 total_ops,
                 total_failures);
        return std::unexpected(sak::error_code::write_error);
    }

    setRandomWriteResults(total_ops, total_bytes, elapsed_sec, write_mbps, iops);

    finalizeLatencies(latencies, avg_latency_us, latencies_out);

    logInfo("Random {}K write QD{}: {:.0f} IOPS, {:.1f} MB/s, {:.0f} us avg",
            m_rand_block_bytes / static_cast<size_t>(sak::kBytesPerKB),
            queue_depth,
            iops,
            write_mbps,
            avg_latency_us);
    return {};
#else
    logWarning("Random 4K write benchmark requires Windows platform");
    (void)queue_depth;
    (void)write_mbps;
    (void)iops;
    (void)avg_latency_us;
    (void)latencies_out;
    return {};
#endif
}

double DiskBenchmarkWorker::calculateP99(std::vector<double>& latencies) const {
    if (latencies.empty()) {
        return 0.0;
    }
    std::sort(latencies.begin(), latencies.end());
    const size_t idx = static_cast<size_t>(latencies.size() * kP99Percentile);
    return latencies[std::min(idx, latencies.size() - 1)];
}

void DiskBenchmarkWorker::calculateScore() {
    // Weighted score based on Samsung 980 PRO = 1000
    // Weights: SeqRead 20%, SeqWrite 20%, Rand4K_QD32 Read 30%, Rand4K_QD32 Write 30%
    const double seq_r = (m_result.seq_read_mbps / kRefSeqReadMbps) * kSeqReadScoreWeight;
    const double seq_w = (m_result.seq_write_mbps / kRefSeqWriteMbps) * kSeqWriteScoreWeight;
    const double rand_r = (m_result.rand_4k_qd32_read_iops / kRefRand4KReadIops) *
                          kRandReadScoreWeight;
    const double rand_w = (m_result.rand_4k_qd32_write_iops / kRefRand4KWriteIops) *
                          kRandWriteScoreWeight;

    m_result.overall_score = static_cast<int>((seq_r + seq_w + rand_r + rand_w) * kDiskScoreScale);
    logInfo("Disk benchmark score: {}", m_result.overall_score);
}

}  // namespace sak
