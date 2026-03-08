// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file stress_test_worker.h
/// @brief Extended stress testing worker for CPU, memory, and disk

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/worker_base.h"
#include "sak/thermal_monitor.h"

#include <QElapsedTimer>
#include <QObject>

#include <atomic>
#include <future>
#include <vector>

namespace sak {

/// Forward declaration for GPU stress resource context (defined in .cpp)
struct GpuStressContext;

/// @brief Runs sustained stress tests on CPU, memory, and/or disk
///
/// Designed for burn-in testing and stability validation. Monitors
/// temperature in real time and auto-aborts if thermal limits are exceeded.
/// Memory stress includes pattern verification to detect ECC/memory errors.
///
/// Usage:
/// @code
///   StressTestWorker worker;
///   StressTestConfig config;
///   config.stress_cpu = true;
///   config.duration_minutes = 10;
///   worker.setConfig(config);
///   connect(&worker, &StressTestWorker::stressTestComplete, ...);
///   worker.start();
/// @endcode
class StressTestWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Construct a StressTestWorker
    /// @param parent Parent QObject
    explicit StressTestWorker(QObject* parent = nullptr);
    ~StressTestWorker() override = default;

    // Non-copyable, non-movable
    StressTestWorker(const StressTestWorker&) = delete;
    StressTestWorker& operator=(const StressTestWorker&) = delete;
    StressTestWorker(StressTestWorker&&) = delete;
    StressTestWorker& operator=(StressTestWorker&&) = delete;

    /// @brief Set the stress test configuration
    /// @param config Test parameters
    void setConfig(const StressTestConfig& config) { m_config = config; }

    /// @brief Get the result from the last completed stress test
    [[nodiscard]] const StressTestResult& result() const { return m_result; }

Q_SIGNALS:
    /// @brief Emitted when the stress test completes (or is aborted)
    /// @param result Complete stress test results
    void stressTestComplete(const sak::StressTestResult& result);

    /// @brief Emitted periodically with live status
    /// @param elapsed_seconds Seconds elapsed since start
    /// @param cpu_temp Current CPU temperature (Â°C)
    /// @param errors_so_far Errors detected so far
    void stressTestStatus(int elapsed_seconds, double cpu_temp, int errors_so_far);

protected:
    /// @brief Execute the stress test
    /// @return Success or error code
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /// @brief Launch CPU, memory, and disk stress threads based on config
    void launchStressThreads(std::vector<std::future<void>>& futures);

    /// @brief Monitor running stress test: report progress, check thermals and errors
    void monitorStressLoop(int total_seconds);

    /// @brief CPU stress: sustained all-core compute load
    void runCpuStress();

    /// @brief Memory stress: pattern write/verify cycle
    /// @return Number of pattern errors detected
    int runMemoryStress();

    /// @brief Disk stress: continuous sequential I/O
    void runDiskStress();

    /// @brief GPU stress: sustained compute shader load via DirectX 11
    void runGpuStress();

    /// @brief Load D3D11 library and create hardware device
    bool initGpuDevice(GpuStressContext& ctx);

    /// @brief Load shader compiler, compile and create compute shader
    bool compileGpuShader(GpuStressContext& ctx);

    /// @brief Create UAV buffer for compute shader output
    bool createGpuUavBuffer(GpuStressContext& ctx);

    /// @brief Run the GPU dispatch loop until cancelled or device removed
    void runGpuDispatchLoop(GpuStressContext& ctx);

    /// @brief Write a single stress-test file, returning error count
    int writeDiskStressFile(void* file_handle, const uint8_t* buf,
                            size_t blockSize, size_t fileSize,
                            uint64_t& total_bytes_written);

    /// @brief Check if a number is prime (with periodic cancellation check)
    [[nodiscard]] bool isPrimeStress(uint64_t candidate) const;

    /// @brief Handle periodic status update during stress test
    /// @return true if the test should abort
    [[nodiscard]] bool handleStatusUpdate(int elapsed_sec, int total_seconds);

    /// @brief Atomically update the maximum observed temperature
    void updateMaxTemperature(double temp) noexcept;

    /// @brief Determine target memory allocation based on system available RAM
    /// @param fallback_bytes Allocation to use if platform detection fails
    /// @return Target bytes to allocate
    [[nodiscard]] size_t determineTargetMemoryBytes(size_t fallback_bytes) const;

    /// @brief Allocate memory for stress testing (platform-specific)
    /// @return Pointer to allocated memory, or nullptr on failure
    [[nodiscard]] static volatile uint64_t* allocateStressMemory(size_t alloc_size);

    /// @brief Free memory previously allocated by allocateStressMemory
    static void freeStressMemory(volatile uint64_t* data);

    StressTestConfig m_config;
    StressTestResult m_result;

    /// @brief Signals child stress threads to stop without marking the
    ///        WorkerBase itself as cancelled (which would emit cancelled()
    ///        instead of finished()).
    std::atomic<bool> m_stop_children{false};

    /// @brief Check if child threads should stop
    [[nodiscard]] bool childrenShouldStop() const noexcept
    {
        return m_stop_children.load(std::memory_order_acquire) || stopRequested();
    }

    std::atomic<int> m_error_count{0};
    std::atomic<double> m_current_temp{0.0};
    std::atomic<double> m_max_temp{0.0};
    QElapsedTimer m_elapsed_timer;
};

} // namespace sak
