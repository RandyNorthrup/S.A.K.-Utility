// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file disk_benchmark_worker.h
/// @brief Disk I/O performance benchmarking worker thread

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/worker_base.h"

#include <QObject>

#include <vector>

namespace sak {

/// @brief Measures sequential and random disk I/O performance
///
/// Uses direct I/O (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH) to
/// bypass OS caching for accurate measurements. Tests sequential read/write
/// with 1 MB blocks and random 4K at queue depths 1 and 32.
///
/// The test file is created at the start and cleaned up on completion or
/// cancellation.
///
/// Usage:
/// @code
///   DiskBenchmarkWorker worker;
///   DiskBenchmarkConfig config;
///   config.drive_path = "C:\\";
///   worker.setConfig(config);
///   connect(&worker, &DiskBenchmarkWorker::benchmarkComplete, ...);
///   worker.start();
/// @endcode
class DiskBenchmarkWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Construct a DiskBenchmarkWorker
    /// @param parent Parent QObject
    explicit DiskBenchmarkWorker(QObject* parent = nullptr);
    ~DiskBenchmarkWorker() override = default;

    /// @brief Set the benchmark configuration
    /// @param config Benchmark parameters (drive, sizes, queue depth, etc.)
    void setConfig(const DiskBenchmarkConfig& config) { m_config = config; }

    /// @brief Get the result from the last completed benchmark
    /// @return Benchmark result (valid only after benchmarkComplete signal)
    [[nodiscard]] const DiskBenchmarkResult& result() const { return m_result; }

Q_SIGNALS:
    /// @brief Emitted when the benchmark suite completes
    /// @param result Complete disk benchmark results
    void benchmarkComplete(const sak::DiskBenchmarkResult& result);

protected:
    /// @brief Execute the disk benchmark suite
    /// @return Success or error code
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /// @brief Create the test file with random data
    /// @return true if file was created successfully
    [[nodiscard]] bool createTestFile();

    /// @brief Remove the test file
    void cleanupTestFile();

    /// @brief Run sequential read benchmark
    void runSequentialRead();

    /// @brief Run sequential write benchmark
    void runSequentialWrite();

    /// @brief Run random 4K read benchmark at the given queue depth
    /// @param queue_depth Number of concurrent I/O operations
    /// @param read_mbps Output: throughput in MB/s
    /// @param iops Output: I/O operations per second
    /// @param avg_latency_us Output: average latency in microseconds
    /// @param latencies_out Optional: raw latency samples for P99 calculation
    void runRandom4KRead(int queue_depth,
                         double& read_mbps,
                         double& iops,
                         double& avg_latency_us,
                         std::vector<double>* latencies_out = nullptr);

    /// @brief Run random 4K write benchmark at the given queue depth
    /// @param queue_depth Number of concurrent I/O operations
    /// @param write_mbps Output: throughput in MB/s
    /// @param iops Output: I/O operations per second
    /// @param avg_latency_us Output: average latency in microseconds
    /// @param latencies_out Optional: raw latency samples for P99 calculation
    void runRandom4KWrite(int queue_depth,
                          double& write_mbps,
                          double& iops,
                          double& avg_latency_us,
                          std::vector<double>* latencies_out = nullptr);

    /// @brief Run all benchmark phases (sequential + random I/O)
    /// @return Success or error code (cancellation)
    auto runAllBenchmarks() -> std::expected<void, sak::error_code>;

    /// Accumulated I/O stats for random benchmark loops
    struct RandomIoStats {
        std::vector<double>& latencies;
        uint64_t& total_ops;
        uint64_t& total_bytes;
    };

    /// Configuration for a random I/O loop iteration
    struct RandomIoLoopConfig {
        int queue_depth;
        uint64_t max_offset;
        int duration_ms;
    };

    /// @brief Inner timing loop for random 4K read benchmark
    double runRandom4KReadLoop(void* file_handle,
                               uint8_t* buf_data,
                               const RandomIoLoopConfig& config,
                               RandomIoStats& stats);

    /// @brief Inner timing loop for random 4K write benchmark
    double runRandom4KWriteLoop(void* file_handle,
                                const uint8_t* buf_data,
                                const RandomIoLoopConfig& config,
                                RandomIoStats& stats);

    /// @brief Execute a single random 4K read I/O operation
    void processRandomReadOp(void* file_handle,
                             uint8_t* buf_data,
                             int queue_index,
                             uint64_t offset,
                             RandomIoStats& stats);

    /// @brief Execute a single random 4K write I/O operation
    void processRandomWriteOp(void* file_handle,
                              const uint8_t* buf_data,
                              int queue_index,
                              uint64_t offset,
                              RandomIoStats& stats);

    /// @brief Execute a single sequential read pass
    /// @return Total bytes read in this pass
    size_t readSequentialPass(void* file_handle,
                              uint8_t* buffer,
                              size_t bufSize,
                              size_t total_bytes);

    /// @brief Execute a single sequential write pass
    /// @return Total bytes written in this pass
    size_t writeSequentialPass(void* file_handle,
                               const uint8_t* buffer,
                               size_t bufSize,
                               size_t total_bytes);

    /// @brief Calculate the P99 latency from a sorted list
    /// @param latencies Sorted vector of latencies
    /// @return P99 latency value
    [[nodiscard]] double calculateP99(std::vector<double>& latencies) const;

    /// @brief Calculate a normalized score
    void calculateScore();

    /// @brief Resolve test file path
    [[nodiscard]] QString testFilePath() const;

    DiskBenchmarkConfig m_config;
    DiskBenchmarkResult m_result;
};

}  // namespace sak
