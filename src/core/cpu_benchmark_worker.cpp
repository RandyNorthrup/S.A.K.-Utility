// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file cpu_benchmark_worker.cpp
/// @brief CPU benchmark suite implementation

#include "sak/cpu_benchmark_worker.h"
#include "sak/logger.h"

#include <QElapsedTimer>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include <zlib.h>

namespace sak {

// ============================================================================
// Baseline reference times (ms) — Intel i5-12400 single-thread
// ============================================================================

namespace {

constexpr double kBaselinePrimeSieveMs      = 120.0;
constexpr double kBaselineMatrixMultiplyMs   = 350.0;
constexpr double kBaselineZlibCompressionMs  = 280.0;
constexpr double kBaselineAesEncryptionMs    = 180.0;

/// AES S-Box (simplified; used for byte-level permutation workload)
alignas(64) constexpr uint8_t kAesSBox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

} // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

CpuBenchmarkWorker::CpuBenchmarkWorker(QObject* parent)
    : WorkerBase(parent)
{
}

// ============================================================================
// WorkerBase Override
// ============================================================================

auto CpuBenchmarkWorker::execute() -> std::expected<void, sak::error_code>
{
    logInfo("Starting CPU benchmark suite");
    m_result = CpuBenchmarkResult{};
    m_result.thread_count = std::thread::hardware_concurrency();

    // ── Single-thread benchmarks ────────────────────────────────
    reportProgress(0, 6, "Running prime sieve benchmark...");
    if (checkStop()) return std::unexpected(sak::error_code::operation_cancelled);
    m_result.prime_sieve_time_ms = runPrimeSieve();

    reportProgress(1, 6, "Running matrix multiplication benchmark...");
    if (checkStop()) return std::unexpected(sak::error_code::operation_cancelled);
    m_result.matrix_multiply_time_ms = runMatrixMultiply();

    reportProgress(2, 6, "Running ZLIB compression benchmark...");
    if (checkStop()) return std::unexpected(sak::error_code::operation_cancelled);
    m_result.zlib_compression_time_ms = runZlibCompression();

    reportProgress(3, 6, "Running AES encryption benchmark...");
    if (checkStop()) return std::unexpected(sak::error_code::operation_cancelled);
    m_result.aes_encryption_time_ms = runAesEncryption();

    // Store throughput metrics
    m_result.zlib_throughput_mbps = m_zlib_throughput_mbps;
    m_result.aes_throughput_mbps  = m_aes_throughput_mbps;

    // Compute GFLOPS from single-thread matrix multiply (512x512)
    {
        const double elapsed_sec = m_result.matrix_multiply_time_ms / 1000.0;
        const double ops = 2.0 * 512.0 * 512.0 * 512.0;
        m_matrix_gflops = (elapsed_sec > 0.0) ? (ops / elapsed_sec / 1e9) : 0.0;
        m_result.matrix_gflops = m_matrix_gflops;
    }

    // ── Multi-thread benchmark ──────────────────────────────────
    reportProgress(4, 6, "Running multi-threaded benchmark...");
    if (checkStop()) return std::unexpected(sak::error_code::operation_cancelled);

    const double st_total = m_result.prime_sieve_time_ms +
                            m_result.matrix_multiply_time_ms +
                            m_result.zlib_compression_time_ms +
                            m_result.aes_encryption_time_ms;

    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    const double mt_total = runMultiThreaded(hw_threads);

    // Thread scaling efficiency
    if (mt_total > 0.0 && hw_threads > 0) {
        m_result.thread_scaling_efficiency = (st_total / mt_total) /
                                              static_cast<double>(hw_threads);
    }

    // ── Scoring ─────────────────────────────────────────────────
    reportProgress(5, 6, "Calculating scores...");
    calculateScores();

    m_result.timestamp = QDateTime::currentDateTime();

    logInfo("CPU benchmark complete — ST: {}, MT: {}",
            m_result.single_thread_score, m_result.multi_thread_score);

    Q_EMIT benchmarkComplete(m_result);

    return {};
}

// ============================================================================
// Individual Benchmarks
// ============================================================================

double CpuBenchmarkWorker::runPrimeSieve(uint64_t limit)
{
    // Sieve of Eratosthenes — stresses integer arithmetic + memory
    QElapsedTimer timer;
    timer.start();

    std::vector<bool> sieve(limit + 1, true);
    sieve[0] = sieve[1] = false;

    const uint64_t sqrt_limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(limit)));

    for (uint64_t i = 2; i <= sqrt_limit; ++i) {
        if (sieve[i]) {
            for (uint64_t j = i * i; j <= limit; j += i) {
                sieve[j] = false;
            }
        }
    }

    // Count primes (prevents optimizer from eliding the work)
    [[maybe_unused]] const auto count = std::count(sieve.begin(), sieve.end(), true);

    const double elapsed = timer.nsecsElapsed() / 1'000'000.0;
    logInfo("Prime sieve ({} primes up to {}) completed in {:.1f} ms",
            count, limit, elapsed);
    return elapsed;
}

double CpuBenchmarkWorker::runMatrixMultiply(int size)
{
    // Dense matrix multiply C = A × B — stresses FP pipeline + cache
    const int n = size;
    std::vector<double> a(n * n);
    std::vector<double> b(n * n);
    std::vector<double> c(n * n, 0.0);

    // Deterministic init with fixed seed for reproducibility
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    for (auto& val : a) val = dist(rng);
    for (auto& val : b) val = dist(rng);

    QElapsedTimer timer;
    timer.start();

    // ikj loop order for cache-friendly access of B
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < n; ++k) {
            const double a_ik = a[i * n + k];
            for (int j = 0; j < n; ++j) {
                c[i * n + j] += a_ik * b[k * n + j];
            }
        }
    }

    const double elapsed = timer.nsecsElapsed() / 1'000'000.0;

    // GFLOPS = 2*N^3 / (time_sec * 10^9)
    const double ops = 2.0 * static_cast<double>(n) * n * n;
    const double gflops = (elapsed > 0.0)
                              ? (ops / (elapsed / 1000.0) / 1e9)
                              : 0.0;

    // Prevent optimizer from eliding c
    volatile double sink = c[0];
    (void)sink;

    logInfo("Matrix {}x{} multiply completed in {:.1f} ms ({:.2f} GFLOPS)",
            n, n, elapsed, gflops);
    return elapsed;
}

double CpuBenchmarkWorker::runZlibCompression(int data_size_mb)
{
    const size_t data_size = static_cast<size_t>(data_size_mb) * 1024 * 1024;

    // Generate compressible data (mixed patterns)
    std::vector<uint8_t> input(data_size);
    std::mt19937 rng(123);
    for (size_t i = 0; i < data_size; ++i) {
        // Mix of random and repeating patterns for realistic compressibility
        if (i % 256 < 128) {
            input[i] = static_cast<uint8_t>(rng() & 0xFF);
        } else {
            input[i] = static_cast<uint8_t>(i & 0xFF);
        }
    }

    // Output buffer (worst case: slightly larger than input)
    uLongf compressed_size = compressBound(static_cast<uLong>(data_size));
    std::vector<uint8_t> output(compressed_size);

    QElapsedTimer timer;
    timer.start();

    const int ret = compress2(
        output.data(), &compressed_size,
        input.data(), static_cast<uLong>(data_size),
        Z_DEFAULT_COMPRESSION);

    const double elapsed = timer.nsecsElapsed() / 1'000'000.0;

    if (ret != Z_OK) {
        logError("ZLIB compression failed with code {}", ret);
        return elapsed;
    }

    const double ratio = static_cast<double>(data_size) /
                          static_cast<double>(compressed_size);
    m_zlib_throughput_mbps = static_cast<double>(data_size) / (1024.0 * 1024.0) /
                             (elapsed / 1000.0);

    logInfo("ZLIB: {} MB compressed in {:.1f} ms ({:.1f} MB/s, {:.2f}x ratio)",
            data_size_mb, elapsed, m_zlib_throughput_mbps, ratio);
    return elapsed;
}

double CpuBenchmarkWorker::runAesEncryption(int data_size_mb)
{
    const size_t data_size = static_cast<size_t>(data_size_mb) * 1024 * 1024;

    // Generate random data
    std::vector<uint8_t> data(data_size);
    std::mt19937 rng(456);
    for (size_t i = 0; i < data_size; ++i) {
        data[i] = static_cast<uint8_t>(rng() & 0xFF);
    }

    // Fixed key schedule (16 bytes for AES-128)
    alignas(16) uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };

    QElapsedTimer timer;
    timer.start();

    // AES-like S-Box substitution with key mixing — representative of
    // real encryption workload without requiring a full AES implementation.
    // Processes data in 16-byte blocks, applying SubBytes + AddRoundKey × 10 rounds.
    for (size_t block = 0; block + 16 <= data_size; block += 16) {
        for (int round = 0; round < 10; ++round) {
            for (int b = 0; b < 16; ++b) {
                data[block + b] = kAesSBox[data[block + b]] ^ key[b];
            }
        }
    }

    const double elapsed = timer.nsecsElapsed() / 1'000'000.0;

    // Prevent optimizer from eliding
    volatile uint8_t sink = data[0];
    (void)sink;

    m_aes_throughput_mbps = static_cast<double>(data_size) / (1024.0 * 1024.0) /
                            (elapsed / 1000.0);

    logInfo("AES: {} MB encrypted in {:.1f} ms ({:.1f} MB/s)",
            data_size_mb, elapsed, m_aes_throughput_mbps);
    return elapsed;
}

double CpuBenchmarkWorker::runMultiThreaded(int thread_count)
{
    // Run all four benchmarks in parallel across thread_count threads.
    // Each thread executes the full benchmark suite; the total wall time is measured.

    QElapsedTimer timer;
    timer.start();

    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<size_t>(thread_count));

    for (int t = 0; t < thread_count; ++t) {
        futures.push_back(std::async(std::launch::async, [this]() {
            // Smaller workloads per thread to keep total duration reasonable
            (void)runPrimeSieve(2'000'000);
            (void)runMatrixMultiply(256);
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    const double elapsed = timer.nsecsElapsed() / 1'000'000.0;
    logInfo("Multi-threaded benchmark ({} threads) completed in {:.1f} ms",
            thread_count, elapsed);
    return elapsed;
}

void CpuBenchmarkWorker::calculateScores()
{
    // Single-thread score: geometric mean of individual ratios x 1000
    const double prime_ratio  = kBaselinePrimeSieveMs / std::max(m_result.prime_sieve_time_ms, 0.001);
    const double matrix_ratio = kBaselineMatrixMultiplyMs / std::max(m_result.matrix_multiply_time_ms, 0.001);
    const double zlib_ratio   = kBaselineZlibCompressionMs / std::max(m_result.zlib_compression_time_ms, 0.001);
    const double aes_ratio    = kBaselineAesEncryptionMs / std::max(m_result.aes_encryption_time_ms, 0.001);

    const double geometric_mean = std::pow(
        prime_ratio * matrix_ratio * zlib_ratio * aes_ratio,
        0.25);

    m_result.single_thread_score = static_cast<int>(geometric_mean * 1000.0);

    // Multi-thread score: single_thread × thread_count × efficiency
    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    m_result.multi_thread_score = static_cast<int>(
        m_result.single_thread_score *
        static_cast<double>(hw_threads) *
        m_result.thread_scaling_efficiency);

    logInfo("Scores — ST: {}, MT: {}, Scaling: {:.1f}%",
            m_result.single_thread_score, m_result.multi_thread_score,
            m_result.thread_scaling_efficiency * 100.0);
}

} // namespace sak
