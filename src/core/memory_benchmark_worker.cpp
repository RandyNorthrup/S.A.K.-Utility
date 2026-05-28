// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file memory_benchmark_worker.cpp
/// @brief Memory bandwidth and latency benchmark implementation

#include "sak/memory_benchmark_worker.h"

#include "sak/action_constants.h"
#include "sak/aligned_buffer.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QElapsedTimer>

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

/// Buffer size for bandwidth tests (256 MB -- large enough to exceed L3 cache)
constexpr size_t kBandwidthBufferSize = 256ULL * 1024 * 1024;

/// Number of passes for bandwidth averaging
constexpr int kBandwidthPasses = 3;

/// Latency test array size (64 MB of pointers -- fits in RAM, exceeds L3)
constexpr size_t kLatencyArrayElements = 8 * 1024 * 1024;

/// Number of pointer chases for latency measurement
constexpr size_t kLatencyChases = 10'000'000;

/// Reference scores (DDR4-3200 dual-channel baseline = 1000)
constexpr double kRefReadGbps = 38.0;
constexpr double kRefWriteGbps = 35.0;
constexpr double kRefCopyGbps = 33.0;
constexpr double kRefLatencyNs = 75.0;
constexpr int kMemoryBenchmarkStepTotal = 5;
constexpr int kMemoryProgressWrite = 1;
constexpr int kMemoryProgressCopy = 2;
constexpr int kMemoryProgressLatency = 3;
constexpr int kMemoryProgressAllocation = 4;
constexpr double kBandwidthScoreWeight = 0.25;
constexpr double kLatencyScoreWeight = 0.25;
constexpr double kMinimumLatencyNs = 1.0;
constexpr double kMemoryScoreScale = 1000.0;
constexpr int kCommittedFillByte = 0xAA;
constexpr int kZeroFillByte = 0x00;
constexpr size_t kBandwidthUnrollFactor = 8;
constexpr uint64_t kWriteBandwidthPattern = 0xDE'AD'BE'EF'CA'FE'BA'BEULL;
constexpr uint64_t kLatencyShuffleSeed = 0xC0'FF'EE;
constexpr size_t kLatencyWarmupChases = 1000;
constexpr size_t kAllocBlockSize = 64 * 1024;
constexpr int kAllocOps = 10'000;
constexpr size_t kAllocationBackoffDivisor = 2;
constexpr double kNanosecondsPerSecond = 1'000'000'000.0;

}  // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

MemoryBenchmarkWorker::MemoryBenchmarkWorker(QObject* parent) : WorkerBase(parent) {}

// ============================================================================
// WorkerBase Override
// ============================================================================

auto MemoryBenchmarkWorker::execute() -> std::expected<void, sak::error_code> {
    logInfo("Starting memory benchmark suite");
    m_result = MemoryBenchmarkResult{};

    if (auto status = runBandwidthBenchmarks(); !status) {
        return status;
    }
    if (auto status = runLatencyAndAllocationBenchmarks(); !status) {
        return status;
    }

    calculateScore();
    m_result.timestamp = QDateTime::currentDateTime();
    emitCompletion();
    return {};
}

auto MemoryBenchmarkWorker::runBandwidthBenchmarks() -> std::expected<void, sak::error_code> {
    reportProgress(sak::progress::kStart, kMemoryBenchmarkStepTotal, "Measuring read bandwidth...");
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    m_result.read_bandwidth_gbps = runReadBandwidth();

    // Write bandwidth
    reportProgress(kMemoryProgressWrite, kMemoryBenchmarkStepTotal, "Measuring write bandwidth...");
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    m_result.write_bandwidth_gbps = runWriteBandwidth();

    // Copy bandwidth
    reportProgress(kMemoryProgressCopy, kMemoryBenchmarkStepTotal, "Measuring copy bandwidth...");
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    m_result.copy_bandwidth_gbps = runCopyBandwidth();
    return {};
}

auto MemoryBenchmarkWorker::runLatencyAndAllocationBenchmarks()
    -> std::expected<void, sak::error_code> {
    reportProgress(kMemoryProgressLatency,
                   kMemoryBenchmarkStepTotal,
                   "Measuring random access latency...");
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    m_result.random_latency_ns = runRandomLatency();

    // Allocation stress
    reportProgress(kMemoryProgressAllocation,
                   kMemoryBenchmarkStepTotal,
                   "Running allocation stress test...");
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    runAllocationStress();
    return {};
}

void MemoryBenchmarkWorker::calculateScore() {
    const double read_score = (m_result.read_bandwidth_gbps / kRefReadGbps) * kBandwidthScoreWeight;
    const double write_score = (m_result.write_bandwidth_gbps / kRefWriteGbps) *
                               kBandwidthScoreWeight;
    const double copy_score = (m_result.copy_bandwidth_gbps / kRefCopyGbps) * kBandwidthScoreWeight;
    // Latency: lower is better, so invert
    const double lat_score =
        (kRefLatencyNs / std::max(m_result.random_latency_ns, kMinimumLatencyNs)) *
        kLatencyScoreWeight;

    m_result.overall_score =
        static_cast<int>((read_score + write_score + copy_score + lat_score) * kMemoryScoreScale);
}

void MemoryBenchmarkWorker::emitCompletion() {
    logInfo(
        "Memory benchmark complete -- R: {:.1f} GB/s, W: {:.1f} GB/s, "
        "Copy: {:.1f} GB/s, Latency: {:.1f} ns, Score: {}",
        m_result.read_bandwidth_gbps,
        m_result.write_bandwidth_gbps,
        m_result.copy_bandwidth_gbps,
        m_result.random_latency_ns,
        m_result.overall_score);

    Q_EMIT benchmarkComplete(m_result);
}

// ============================================================================
// Bandwidth Tests
// ============================================================================

double MemoryBenchmarkWorker::runReadBandwidth() {
    VirtualBuffer buf(kBandwidthBufferSize);
    if (!buf.valid()) {
        logError("Failed to allocate {} MB for read bandwidth test",
                 kBandwidthBufferSize / sak::kBytesPerMB);
        return 0.0;
    }

    // Initialize to force page commit
    std::memset(buf.data(), kCommittedFillByte, buf.size());

    const size_t count = buf.size() / sizeof(uint64_t);
    const auto* data = buf.as<volatile uint64_t>();

    double best_gbps = 0.0;

    for (int pass = 0; pass < kBandwidthPasses; ++pass) {
        QElapsedTimer timer;
        timer.start();

        uint64_t sum = 0;
        for (size_t i = 0; i < count; i += kBandwidthUnrollFactor) {
            // Unrolled reads to saturate memory bus
            for (size_t offset = 0; offset < kBandwidthUnrollFactor; ++offset) {
                sum += data[i + offset];
            }
        }

        const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
        const double gbps = static_cast<double>(buf.size()) / sak::kBytesPerGBf / elapsed_sec;
        best_gbps = std::max(best_gbps, gbps);

        // Prevent optimization from eliding reads
        volatile uint64_t sink = sum;
        (void)sink;
    }

    logInfo("Memory read bandwidth: {:.1f} GB/s", best_gbps);
    return best_gbps;
}

double MemoryBenchmarkWorker::runWriteBandwidth() {
    VirtualBuffer buf(kBandwidthBufferSize);
    if (!buf.valid()) {
        return 0.0;
    }

    // Force page commit
    std::memset(buf.data(), 0, buf.size());

    const size_t count = buf.size() / sizeof(uint64_t);
    auto* data = buf.as<volatile uint64_t>();

    double best_gbps = 0.0;

    for (int pass = 0; pass < kBandwidthPasses; ++pass) {
        QElapsedTimer timer;
        timer.start();

        for (size_t i = 0; i < count; i += kBandwidthUnrollFactor) {
            for (size_t offset = 0; offset < kBandwidthUnrollFactor; ++offset) {
                data[i + offset] = kWriteBandwidthPattern;
            }
        }

        const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
        const double gbps = static_cast<double>(buf.size()) / sak::kBytesPerGBf / elapsed_sec;
        best_gbps = std::max(best_gbps, gbps);
    }

    logInfo("Memory write bandwidth: {:.1f} GB/s", best_gbps);
    return best_gbps;
}

double MemoryBenchmarkWorker::runCopyBandwidth() {
    VirtualBuffer src(kBandwidthBufferSize);
    VirtualBuffer dst(kBandwidthBufferSize);
    if (!src.valid() || !dst.valid()) {
        return 0.0;
    }

    // Initialize
    std::memset(src.data(), kCommittedFillByte, src.size());
    std::memset(dst.data(), kZeroFillByte, dst.size());

    double best_gbps = 0.0;

    for (int pass = 0; pass < kBandwidthPasses; ++pass) {
        QElapsedTimer timer;
        timer.start();

        std::memcpy(dst.data(), src.data(), src.size());

        const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
        // Copy touches both read and write = 2x buffer size
        const double gbps = static_cast<double>(src.size()) / sak::kBytesPerGBf / elapsed_sec;
        best_gbps = std::max(best_gbps, gbps);
    }

    logInfo("Memory copy bandwidth: {:.1f} GB/s", best_gbps);
    return best_gbps;
}

// ============================================================================
// Latency Test
// ============================================================================

double MemoryBenchmarkWorker::runRandomLatency() {
    // Pointer-chase: create a randomized linked list in memory.
    // Each element points to a random other element. Sequential
    // prefetchers cannot predict the next access -> measures true latency.

    const size_t element_count = kLatencyArrayElements;
    VirtualBuffer buf(element_count * sizeof(size_t));
    if (!buf.valid()) {
        return 0.0;
    }

    auto* arr = buf.as<size_t>();

    // Create a random permutation for the chase sequence
    std::vector<size_t> indices(element_count);
    std::iota(indices.begin(), indices.end(), 0);

    // Fisher-Yates shuffle with fixed seed
    std::mt19937_64 rng(kLatencyShuffleSeed);
    for (size_t i = element_count - 1; i > 0; --i) {
        std::uniform_int_distribution<size_t> dist(0, i);
        std::swap(indices[i], indices[dist(rng)]);
    }

    // Build the pointer-chase chain: arr[indices[i]] = indices[i+1]
    for (size_t i = 0; i < element_count - 1; ++i) {
        arr[indices[i]] = indices[i + 1];
    }
    arr[indices[element_count - 1]] = indices[0];  // Close the loop

    // Warm up
    size_t idx = 0;
    for (size_t i = 0; i < kLatencyWarmupChases; ++i) {
        idx = arr[idx];
    }

    // Timed chase
    QElapsedTimer timer;
    timer.start();

    for (size_t i = 0; i < kLatencyChases; ++i) {
        idx = arr[idx];
    }

    const double elapsed_ns = static_cast<double>(timer.nsecsElapsed());
    const double latency_ns = elapsed_ns / static_cast<double>(kLatencyChases);

    // Prevent optimization
    volatile size_t sink = idx;
    (void)sink;

    logInfo("Memory random latency: {:.1f} ns", latency_ns);
    return latency_ns;
}

// ============================================================================
// Allocation Stress
// ============================================================================

void MemoryBenchmarkWorker::runAllocationStress() {
    // Test how fast the system can allocate and free memory blocks

    QElapsedTimer timer;
    timer.start();

    uint64_t max_contiguous = 0;

    for (int i = 0; i < kAllocOps; ++i) {
        void* ptr = std::malloc(kAllocBlockSize);
        if (ptr) {
            std::memset(ptr, kZeroFillByte, kAllocBlockSize);
            std::free(ptr);
        }
    }

    const double elapsed_sec = timer.nsecsElapsed() / kNanosecondsPerSecond;
    m_result.alloc_dealloc_ops_per_sec = static_cast<double>(kAllocOps) / elapsed_sec;

    // Find max contiguous allocation
    size_t test_size = sak::kBytesPerGB;  // Start at 1 GB
    while (test_size >= sak::kBytesPerMB) {
        void* ptr = std::malloc(test_size);
        if (ptr) {
            max_contiguous = test_size / sak::kBytesPerMB;
            std::free(ptr);
            break;
        }
        test_size /= kAllocationBackoffDivisor;
    }

    m_result.max_contiguous_alloc_mb = max_contiguous;

    logInfo("Allocation stress: {:.0f} ops/s, max contiguous: {} MB",
            m_result.alloc_dealloc_ops_per_sec,
            max_contiguous);
}

}  // namespace sak
