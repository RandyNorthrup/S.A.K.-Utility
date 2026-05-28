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

#include <QDir>
#include <QElapsedTimer>
#include <QStorageInfo>
#include <QtGlobal>

#include <algorithm>
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
constexpr size_t kSequentialBlockSize = sak::kBytesPerMB;  // 1 MB
constexpr size_t kRandomBlockSize = 4096;                  // 4 KB
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
HANDLE openRandomWriteFile(const std::wstring& path) {
    return CreateFileW(path.c_str(),
                       GENERIC_WRITE,
                       0,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_RANDOM_ACCESS,
                       nullptr);
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

    logInfo("Starting disk benchmark on {}", m_config.drive_path.toStdString());
    m_result = DiskBenchmarkResult{};
    m_result.drive_path = m_config.drive_path;

    // Resolve drive info
    QStorageInfo storage(m_config.drive_path);
    if (storage.isValid()) {
        m_result.drive_capacity_bytes = static_cast<uint64_t>(storage.bytesTotal());
    }

    // Create test file
    reportProgress(sak::progress::kStart, kDiskBenchmarkStepTotal, "Creating test file...");
    if (!createTestFile()) {
        return std::unexpected(sak::error_code::read_error);
    }

    auto benchmark_result = runAllBenchmarks();
    if (!benchmark_result) {
        return benchmark_result;
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
    runSequentialRead();

    reportProgress(kDiskProgressSequentialWrite,
                   kDiskBenchmarkStepTotal,
                   "Sequential write benchmark...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    runSequentialWrite();
    return {};
}

auto DiskBenchmarkWorker::runRandomQd1Benchmarks() -> std::expected<void, sak::error_code> {
    reportProgress(kDiskProgressRandomQd1Read, kDiskBenchmarkStepTotal, "Random 4K QD1 read...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    runRandom4KRead(m_config.queue_depth_low,
                    m_result.rand_4k_read_mbps,
                    m_result.rand_4k_read_iops,
                    m_result.avg_read_latency_us);

    reportProgress(kDiskProgressRandomQd1Write, kDiskBenchmarkStepTotal, "Random 4K QD1 write...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    runRandom4KWrite(m_config.queue_depth_low,
                     m_result.rand_4k_write_mbps,
                     m_result.rand_4k_write_iops,
                     m_result.avg_write_latency_us);
    return {};
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
    runRandom4KRead(m_config.queue_depth_high,
                    m_result.rand_4k_qd32_read_mbps,
                    m_result.rand_4k_qd32_read_iops,
                    qd32_avg_read_lat,
                    &qd32_read_latencies);
    m_result.p99_read_latency_us = calculateP99(qd32_read_latencies);

    reportProgress(kDiskProgressRandomQd32Write,
                   kDiskBenchmarkStepTotal,
                   "Random 4K QD32 write...");
    if (auto status = continueOrCancelBenchmark(); !status) {
        return status;
    }
    runRandom4KWrite(m_config.queue_depth_high,
                     m_result.rand_4k_qd32_write_mbps,
                     m_result.rand_4k_qd32_write_iops,
                     qd32_avg_write_lat,
                     &qd32_write_latencies);
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

QString DiskBenchmarkWorker::testFilePath() const {
    return QDir(m_config.drive_path).filePath("sak_disk_benchmark.tmp");
}

bool DiskBenchmarkWorker::createTestFile() {
#ifdef SAK_PLATFORM_WINDOWS
    const QString path = testFilePath();
    const std::wstring wpath = path.toStdWString();

    // Create with direct I/O flags
    HANDLE h =
        CreateFileW(wpath.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_ALWAYS,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Failed to create test file: {}", path.toStdString());
        return false;
    }

    const size_t total_bytes = static_cast<size_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    AlignedBuffer buf(kSequentialBlockSize, kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return false;
    }

    // Fill with pseudo-random data (fixed seed for reproducibility)
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

    CloseHandle(h);
    logInfo("Test file created: {} ({} MB)", path.toStdString(), m_config.test_file_size_mb);
    return true;
#else
    return false;
#endif
}

void DiskBenchmarkWorker::cleanupTestFile() {
    const QString path = testFilePath();
    if (QFile::exists(path)) {
        QFile::remove(path);
        logInfo("Test file removed: {}", path.toStdString());
    }
}

// ============================================================================
// Sequential Benchmarks
// ============================================================================

void DiskBenchmarkWorker::runSequentialRead() {
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    HANDLE h = CreateFileW(wpath.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Sequential read: Failed to open test file");
        return;
    }

    AlignedBuffer buf(kSequentialBlockSize, kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return;
    }

    const size_t total_bytes = static_cast<size_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    double best_mbps = 0.0;

    for (int pass = 0; pass < m_config.sequential_passes; ++pass) {
        QElapsedTimer timer;
        timer.start();

        size_t bytes_read_total = readSequentialPass(h, buf.data(), buf.size(), total_bytes);

        const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
        const double mbps = static_cast<double>(bytes_read_total) / sak::kBytesPerMBf / elapsed_sec;
        best_mbps = std::max(best_mbps, mbps);
    }

    CloseHandle(h);
    m_result.seq_read_mbps = best_mbps;
    logInfo("Sequential read: {:.0f} MB/s", best_mbps);
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

void DiskBenchmarkWorker::runSequentialWrite() {
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    AlignedBuffer buf(kSequentialBlockSize, kSectorAlignment);
    if (!buf.valid()) {
        return;
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
        HANDLE h = CreateFileW(wpath.c_str(),
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                               nullptr);

        if (h == INVALID_HANDLE_VALUE) {
            logError("Sequential write: Failed to open test file");
            return;
        }

        QElapsedTimer timer;
        timer.start();

        size_t written_total = writeSequentialPass(h, buf.data(), buf.size(), total_bytes);

        FlushFileBuffers(h);
        const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
        const double mbps = static_cast<double>(written_total) / sak::kBytesPerMBf / elapsed_sec;
        best_mbps = std::max(best_mbps, mbps);

        CloseHandle(h);
    }

    m_result.seq_write_mbps = best_mbps;
    logInfo("Sequential write: {:.0f} MB/s", best_mbps);
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

void DiskBenchmarkWorker::runRandom4KRead(int queue_depth,
                                          double& read_mbps,
                                          double& iops,
                                          double& avg_latency_us,
                                          std::vector<double>* latencies_out) {
    Q_ASSERT_X(queue_depth > 0, "runRandom4KRead", "queue_depth must be positive");
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    HANDLE h = CreateFileW(wpath.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS,
                           nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Random read: Failed to open test file");
        return;
    }

    AlignedBuffer buf(kRandomBlockSize * static_cast<size_t>(queue_depth), kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return;
    }

    const uint64_t file_size = static_cast<uint64_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    const uint64_t max_offset = (file_size / kRandomBlockSize - 1) * kRandomBlockSize;
    const int duration_ms = m_config.random_duration_sec * kBenchmarkMillisecondsPerSecond;

    std::vector<double> latencies;
    latencies.reserve(kLatencySampleReserve);
    uint64_t total_ops = 0;
    uint64_t total_bytes = 0;
    RandomIoStats stats{latencies, total_ops, total_bytes};
    const RandomIoLoopConfig loop_config{queue_depth, max_offset, duration_ms};

    const double elapsed_sec = runRandom4KReadLoop(h, buf.data(), loop_config, stats);

    CloseHandle(h);

    iops = static_cast<double>(total_ops) / elapsed_sec;
    read_mbps = static_cast<double>(total_bytes) / sak::kBytesPerMBf / elapsed_sec;

    if (!latencies.empty()) {
        avg_latency_us = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                         static_cast<double>(latencies.size());
    }

    if (latencies_out) {
        *latencies_out = std::move(latencies);
    }

    logInfo("Random 4K read QD{}: {:.0f} IOPS, {:.1f} MB/s, {:.0f} us avg",
            queue_depth,
            iops,
            read_mbps,
            avg_latency_us);
#else
    logWarning("Random 4K read benchmark requires Windows platform");
    (void)queue_depth;
    (void)read_mbps;
    (void)iops;
    (void)avg_latency_us;
    (void)latencies_out;
#endif
}

#ifdef SAK_PLATFORM_WINDOWS
void DiskBenchmarkWorker::processRandomReadOp(
    void* file_handle, uint8_t* buf_data, int queue_index, uint64_t offset, RandomIoStats& stats) {
    HANDLE h = static_cast<HANDLE>(file_handle);

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        logWarning("Random 4K read: SetFilePointerEx failed at offset {} (error {})",
                   offset,
                   GetLastError());
        return;
    }

    QElapsedTimer op_timer;
    op_timer.start();

    DWORD bytes_read = 0;
    if (!ReadFile(h,
                  buf_data + queue_index * kRandomBlockSize,
                  static_cast<DWORD>(kRandomBlockSize),
                  &bytes_read,
                  nullptr)) {
        logWarning("Random 4K read: ReadFile failed at offset {} (error {})",
                   offset,
                   GetLastError());
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
    std::uniform_int_distribution<uint64_t> offset_dist(0, config.max_offset / kRandomBlockSize);

    QElapsedTimer total_timer;
    total_timer.start();

    while (total_timer.elapsed() < config.duration_ms) {
        if (stopRequested()) {
            break;
        }

        for (int queue_index = 0; queue_index < config.queue_depth; ++queue_index) {
            const uint64_t offset = offset_dist(rng) * kRandomBlockSize;
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
        logWarning("Random 4K write: SetFilePointerEx failed at offset {} (error {})",
                   offset,
                   GetLastError());
        return;
    }

    QElapsedTimer op_timer;
    op_timer.start();

    DWORD bytes_written = 0;
    if (!WriteFile(h,
                   buf_data + queue_index * kRandomBlockSize,
                   static_cast<DWORD>(kRandomBlockSize),
                   &bytes_written,
                   nullptr)) {
        logWarning("Random 4K write: WriteFile failed at offset {} (error {})",
                   offset,
                   GetLastError());
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
    std::uniform_int_distribution<uint64_t> offset_dist(0, config.max_offset / kRandomBlockSize);

    QElapsedTimer total_timer;
    total_timer.start();

    while (total_timer.elapsed() < config.duration_ms) {
        if (stopRequested()) {
            break;
        }

        for (int queue_index = 0; queue_index < config.queue_depth; ++queue_index) {
            const uint64_t offset = offset_dist(rng) * kRandomBlockSize;
            processRandomWriteOp(file_handle, buf_data, queue_index, offset, stats);
        }
    }

    return total_timer.nsecsElapsed() / kNanosecondsPerSecond;
}
#endif

void DiskBenchmarkWorker::runRandom4KWrite(int queue_depth,
                                           double& write_mbps,
                                           double& iops,
                                           double& avg_latency_us,
                                           std::vector<double>* latencies_out) {
    Q_ASSERT_X(queue_depth > 0, "runRandom4KWrite", "queue_depth must be positive");
#ifdef SAK_PLATFORM_WINDOWS
    const std::wstring wpath = testFilePath().toStdWString();

    HANDLE h = openRandomWriteFile(wpath);

    if (h == INVALID_HANDLE_VALUE) {
        logError("Random write: Failed to open test file");
        return;
    }

    AlignedBuffer buf(kRandomBlockSize * static_cast<size_t>(queue_depth), kSectorAlignment);
    if (!buf.valid()) {
        CloseHandle(h);
        return;
    }

    fillRandomWriteBuffer(buf);

    const uint64_t file_size = static_cast<uint64_t>(m_config.test_file_size_mb) * sak::kBytesPerMB;
    const uint64_t max_offset = (file_size / kRandomBlockSize - 1) * kRandomBlockSize;

    std::vector<double> latencies;
    latencies.reserve(kLatencySampleReserve);
    uint64_t total_ops = 0;
    uint64_t total_bytes = 0;
    RandomIoStats stats{latencies, total_ops, total_bytes};
    const RandomIoLoopConfig loop_config{
        queue_depth, max_offset, m_config.random_duration_sec * kBenchmarkMillisecondsPerSecond};

    const double elapsed_sec = runRandom4KWriteLoop(h, buf.data(), loop_config, stats);

    CloseHandle(h);
    setRandomWriteResults(total_ops, total_bytes, elapsed_sec, write_mbps, iops);

    if (!latencies.empty()) {
        avg_latency_us = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                         static_cast<double>(latencies.size());
    }

    if (latencies_out) {
        *latencies_out = std::move(latencies);
    }

    logInfo("Random 4K write QD{}: {:.0f} IOPS, {:.1f} MB/s, {:.0f} us avg",
            queue_depth,
            iops,
            write_mbps,
            avg_latency_us);
#else
    logWarning("Random 4K write benchmark requires Windows platform");
    (void)queue_depth;
    (void)write_mbps;
    (void)iops;
    (void)avg_latency_us;
    (void)latencies_out;
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
