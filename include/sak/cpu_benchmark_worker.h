// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file cpu_benchmark_worker.h
/// @brief CPU performance benchmarking worker thread

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/worker_base.h"

#include <QObject>

#include <expected>

namespace sak {

/// @brief Runs CPU benchmarks (prime sieve, matrix multiply, ZLIB, AES)
///
/// Executes a suite of CPU-bound tests in both single-threaded and
/// multi-threaded configurations, producing normalized scores. Uses
/// WorkerBase for thread management and cancellation support.
///
/// Scoring is normalized to an Intel i5-12400 baseline = 1000 points.
///
/// Usage:
/// @code
///   CpuBenchmarkWorker worker;
///   connect(&worker, &CpuBenchmarkWorker::benchmarkComplete, ...);
///   worker.start();
/// @endcode
class CpuBenchmarkWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Construct a CpuBenchmarkWorker
    /// @param parent Parent QObject
    explicit CpuBenchmarkWorker(QObject* parent = nullptr);
    ~CpuBenchmarkWorker() override = default;

    /// @brief Get the result from the last completed benchmark
    /// @return Benchmark result (valid only after benchmarkComplete signal)
    [[nodiscard]] const CpuBenchmarkResult& result() const { return m_result; }

Q_SIGNALS:
    /// @brief Emitted when the benchmark suite completes
    /// @param result Complete benchmark results with scores
    void benchmarkComplete(const sak::CpuBenchmarkResult& result);

protected:
    /// @brief Execute the CPU benchmark suite
    /// @return Success or error code
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    static constexpr uint64_t kDefaultPrimeSieveLimit = 10'000'000;
    static constexpr int kDefaultMatrixSize = 512;
    static constexpr int kDefaultZlibDataSizeMb = 64;
    static constexpr int kDefaultAesDataSizeMb = 256;

    /// @brief Run the prime number sieve benchmark
    /// @param limit Upper bound for sieve (default: 10,000,000)
    /// @return Execution time in milliseconds
    [[nodiscard]] double runPrimeSieve(uint64_t limit = kDefaultPrimeSieveLimit);

    /// @brief Run dense matrix multiplication benchmark (single-thread)
    /// @param size Matrix dimension NxN (default: 512)
    /// @return Execution time in milliseconds
    [[nodiscard]] double runMatrixMultiply(int size = kDefaultMatrixSize);

    /// @brief Run ZLIB in-memory compression benchmark
    /// @param data_size_mb Data size in MB (default: 64)
    /// @return Execution time in milliseconds; also sets m_zlib_throughput
    [[nodiscard]] double runZlibCompression(int data_size_mb = kDefaultZlibDataSizeMb);

    /// @brief Run AES-like byte-shuffling encryption benchmark
    /// @param data_size_mb Data size in MB (default: 256)
    /// @return Execution time in milliseconds; also sets m_aes_throughput
    [[nodiscard]] double runAesEncryption(int data_size_mb = kDefaultAesDataSizeMb);

    /// @brief Run the multi-threaded benchmark pass
    /// @param thread_count Number of worker threads
    /// @return Composite time for all threads
    [[nodiscard]] double runMultiThreaded(int thread_count);

    [[nodiscard]] std::expected<void, sak::error_code> runSingleThreadBenchmarks();
    [[nodiscard]] std::expected<void, sak::error_code> runMultiThreadBenchmark(double st_total);
    void updateDerivedMetrics();

    /// @brief Calculate normalized scores from raw timing data
    void calculateScores();

    CpuBenchmarkResult m_result;
    double m_zlib_throughput_mbps{0.0};
    double m_aes_throughput_mbps{0.0};
    double m_matrix_gflops{0.0};
};

}  // namespace sak
