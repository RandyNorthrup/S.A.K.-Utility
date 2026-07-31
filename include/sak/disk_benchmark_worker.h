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
    // Join the worker thread while this class's members are still alive (the base
    // ~WorkerBase runs after they are destroyed). See WorkerBase::stopAndJoin.
    ~DiskBenchmarkWorker() override { stopAndJoin(); }

    /// @brief Set the benchmark configuration
    /// @param config Benchmark parameters (drive, sizes, queue depth, etc.)
    void setConfig(const DiskBenchmarkConfig& config) { m_config = config; }

    /// @brief Get the result from the last completed benchmark
    /// @return Benchmark result (valid only after benchmarkComplete signal)
    [[nodiscard]] const DiskBenchmarkResult& result() const { return m_result; }

    /// @brief Build a unique, per-run benchmark temp-file name.
    /// @param pid current process id.
    /// @param msecs a millisecond timestamp.
    /// @param counter a monotonically increasing per-run counter.
    /// @return e.g. "sak_disk_benchmark_<pid>_<msecs>_<counter>.tmp".
    /// @note Pure + static for unit testing. A unique name (plus exclusive
    ///       CREATE_NEW) removes the fixed-name TOCTOU: no pre-existing file or
    ///       planted link at a guessable path is truncated/deleted, and
    ///       concurrent runs do not collide.
    [[nodiscard]] static QString makeUniqueBenchmarkFileName(quint32 pid,
                                                             qint64 msecs,
                                                             quint64 counter);

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
    /// @return Success, or an error code if the test file could not be opened/allocated
    auto runSequentialRead() -> std::expected<void, sak::error_code>;

    /// @brief Run sequential write benchmark
    /// @return Success, or an error code if the test file could not be opened/allocated
    auto runSequentialWrite() -> std::expected<void, sak::error_code>;

    /// @brief Run random 4K read benchmark at the given queue depth
    /// @param queue_depth Number of concurrent I/O operations
    /// @param read_mbps Output: throughput in MB/s
    /// @param iops Output: I/O operations per second
    /// @param avg_latency_us Output: average latency in microseconds
    /// @param latencies_out Optional: raw latency samples for P99 calculation
    auto runRandom4KRead(int queue_depth,
                         double& read_mbps,
                         double& iops,
                         double& avg_latency_us,
                         std::vector<double>* latencies_out = nullptr)
        -> std::expected<void, sak::error_code>;

    /// @brief Run random 4K write benchmark at the given queue depth
    /// @param queue_depth Number of concurrent I/O operations
    /// @param write_mbps Output: throughput in MB/s
    /// @param iops Output: I/O operations per second
    /// @param avg_latency_us Output: average latency in microseconds
    /// @param latencies_out Optional: raw latency samples for P99 calculation
    auto runRandom4KWrite(int queue_depth,
                          double& write_mbps,
                          double& iops,
                          double& avg_latency_us,
                          std::vector<double>* latencies_out = nullptr)
        -> std::expected<void, sak::error_code>;

    /// @brief Run all benchmark phases (sequential + random I/O)
    /// @return Success or error code (cancellation)
    auto runAllBenchmarks() -> std::expected<void, sak::error_code>;
    auto runSequentialBenchmarks() -> std::expected<void, sak::error_code>;
    auto runRandomQd1Benchmarks() -> std::expected<void, sak::error_code>;
    auto runRandomQd32Benchmarks() -> std::expected<void, sak::error_code>;
    auto continueOrCancelBenchmark() -> std::expected<void, sak::error_code>;

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

    /// @brief The claimed test file path for this run (set by createTestFile).
    /// @return The unique path createTestFile exclusively claimed, or an empty
    ///         string before a file has been claimed.
    [[nodiscard]] QString testFilePath() const;

    DiskBenchmarkConfig m_config;
    DiskBenchmarkResult m_result;

    /// @brief Path exclusively claimed by createTestFile for this run. Empty
    ///        until a file is claimed; every read/write/cleanup uses it so the
    ///        benchmark never touches a fixed, guessable path.
    QString m_test_file_path;
};

}  // namespace sak
