// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file memory_benchmark_worker.h
/// @brief Memory bandwidth and latency benchmarking worker thread

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/worker_base.h"

#include <QObject>

namespace sak {

/// @brief Measures memory bandwidth (read/write/copy) and latency
///
/// Uses large buffers allocated via VirtualAlloc to bypass small-object
/// allocators. Read/write benchmarks use streaming patterns similar to
/// the STREAM benchmark. Latency test uses pointer-chase to defeat
/// hardware prefetching.
///
/// Usage:
/// @code
///   MemoryBenchmarkWorker worker;
///   connect(&worker, &MemoryBenchmarkWorker::benchmarkComplete, ...);
///   worker.start();
/// @endcode
class MemoryBenchmarkWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Construct a MemoryBenchmarkWorker
    /// @param parent Parent QObject
    explicit MemoryBenchmarkWorker(QObject* parent = nullptr);
    ~MemoryBenchmarkWorker() override = default;

    /// @brief Get the result from the last completed benchmark
    /// @return Benchmark result (valid only after benchmarkComplete signal)
    [[nodiscard]] const MemoryBenchmarkResult& result() const { return m_result; }

Q_SIGNALS:
    /// @brief Emitted when the memory benchmark completes
    /// @param result Complete benchmark results
    void benchmarkComplete(const sak::MemoryBenchmarkResult& result);

protected:
    /// @brief Execute the memory benchmark suite
    /// @return Success or error code
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /// @brief Run streaming read bandwidth test
    /// @return Bandwidth in GB/s
    [[nodiscard]] double runReadBandwidth();

    /// @brief Run streaming write bandwidth test
    /// @return Bandwidth in GB/s
    [[nodiscard]] double runWriteBandwidth();

    /// @brief Run streaming copy bandwidth test (read + write)
    /// @return Bandwidth in GB/s
    [[nodiscard]] double runCopyBandwidth();

    /// @brief Run pointer-chase latency test
    /// @return Random access latency in nanoseconds
    [[nodiscard]] double runRandomLatency();

    /// @brief Run allocation stress test
    void runAllocationStress();

    MemoryBenchmarkResult m_result;
};

}  // namespace sak
