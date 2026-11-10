// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file stress_test_worker.h
/// @brief Extended stress testing worker for CPU, memory, and disk

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/thermal_monitor.h"
#include "sak/worker_base.h"

#include <QElapsedTimer>
#include <QObject>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace sak {

/// Forward declaration for GPU stress resource context (defined in .cpp)
struct GpuStressContext;

class DedicatedThreads;

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
    // Join the worker thread while this class's members are still alive (the base
    // ~WorkerBase runs after they are destroyed). See WorkerBase::stopAndJoin.
    ~StressTestWorker() override { stopAndJoin(); }

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

    /// @brief Compute the pass/fail verdict for a completed stress result.
    /// @details Fails closed on an abort reason, on aggregate errors_detected, or on ANY recorded
    ///          per-component error (disk/memory/gpu); disk_errors in particular never feed
    ///          errors_detected, so they must be checked here for the verdict to be authoritative.
    [[nodiscard]] static bool computeStressPassed(const StressTestResult& result);

    /// @brief Resolve the number of CPU stress worker threads to launch.
    /// @param configThreads requested count; <= 0 means "use all logical CPUs".
    /// @param hwConcurrency std::thread::hardware_concurrency() (0 if unknown).
    /// @return at least 1, and at most an internal ceiling. A requested CPU stress must never
    ///         launch zero threads (that would skip the component while still reporting PASSED),
    ///         and a bogus/hostile positive count is clamped so it cannot spawn unbounded threads.
    /// @note Pure + static for unit testing.
    [[nodiscard]] static int resolveCpuThreadCount(int configThreads, unsigned int hwConcurrency);

    /// @brief What one write-then-verify memory pass did before it ended or was stopped.
    struct MemoryPassOutcome {
        size_t words_written{0};  ///< Words of the buffer filled with this pass's pattern
        size_t words_checked{0};  ///< Words compared against the pattern
        size_t errors{0};         ///< Mismatches among the words checked
        bool completed{false};    ///< The whole buffer was written and checked
    };

    /// @brief Fill @p count words with @p seed's pattern, then check them, a chunk at a time.
    /// @details @p should_stop is asked before every chunk, so a stop request or the end of the
    ///          run is seen within one chunk however large the buffer is. Pure; testable.
    [[nodiscard]] static MemoryPassOutcome memoryPatternPass(
        volatile uint64_t* data,
        size_t count,
        uint64_t seed,
        const std::function<bool()>& should_stop);

    /// @brief Run @p work on the calling thread at the priority every stress thread uses.
    static void runAtStressPriority(const std::function<void()>& work);

    /// @brief Reads the CPU temperature, or -1.0 for no reading. The argument is asked while the
    ///        read runs; true means the run is ending and the read should give up.
    using CpuTemperatureReader = std::function<double(const std::function<bool()>&)>;

Q_SIGNALS:
    /// @brief Emitted when the stress test completes (or is aborted)
    /// @param result Complete stress test results
    void stressTestComplete(const sak::StressTestResult& result);

    /// @brief Emitted periodically with live status
    /// @param elapsed_seconds Seconds elapsed since start
    /// @param cpu_temp Current CPU temperature ( degC)
    /// @param errors_so_far Errors detected so far
    void stressTestStatus(int elapsed_seconds, double cpu_temp, int errors_so_far);

protected:
    /// @brief Execute the stress test
    /// @return Success or error code
    auto execute() -> std::expected<void, sak::error_code> override;

#ifdef SAK_CORE_TEST_HOOKS
public:
    /// @brief Test-only: open the file a disk stress run writes to, let it delete itself, and say
    ///        where it was. Nothing is written to it.
    [[nodiscard]] static QString stressFilePathForTesting(const QString& drive);

    /// @brief Test-only: execute() on the calling thread, for @p run_length instead of the
    ///        configured whole minutes, reading the CPU temperature with @p read_temperature.
    std::expected<void, sak::error_code> executeForTesting(std::chrono::milliseconds run_length,
                                                           CpuTemperatureReader read_temperature);
#endif  // SAK_CORE_TEST_HOOKS

private:
    /// @brief Refuse a config that would do no work, overflow the run's clock, or feed an
    ///        out-of-range value to a component; logs the refused config.
    [[nodiscard]] std::expected<void, sak::error_code> checkConfig() const;

    /// @brief Run a validated config for @p run_length, then report the result.
    std::expected<void, sak::error_code> runFor(std::chrono::milliseconds run_length,
                                                const CpuTemperatureReader& read_temperature);

    /// @brief Launch the stress threads, run the monitor loop, and join every child.
    /// @return Success, or internal_error if launching or running the threads threw.
    /// @details Fails closed on any exception: signals the children to stop and joins every
    ///          one before returning, so none outlives this worker's state.
    std::expected<void, sak::error_code> runStressThreadsToCompletion(
        std::chrono::milliseconds run_length, const CpuTemperatureReader& read_temperature);

    /// @brief Launch the configured stress threads and the temperature reader
    void launchStressThreads(DedicatedThreads& threads,
                             const CpuTemperatureReader& read_temperature);

    /// @brief Monitor running stress test: report progress, check thermals and errors
    void monitorStressLoop(std::chrono::milliseconds run_length);

    /// @brief Read the CPU temperature every status interval until the run ends. Runs on a
    ///        thread of its own: a read starts a process, and a slow one must not hold the
    ///        run's clock.
    void runThermalReader(const CpuTemperatureReader& read_temperature);

    /// @brief Sleep for @p period, or less if the children are told to stop meanwhile.
    void sleepUnlessChildrenStop(std::chrono::milliseconds period) const;

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
    int writeDiskStressFile(void* file_handle,
                            const uint8_t* buf,
                            size_t blockSize,
                            size_t fileSize,
                            uint64_t& total_bytes_written);

    /// @brief Check if a number is prime (with periodic cancellation check)
    [[nodiscard]] bool isPrimeStress(uint64_t candidate) const;

    /// @brief Handle periodic status update during stress test
    /// @return true if the test should abort
    [[nodiscard]] bool handleStatusUpdate(int elapsed_sec, int total_seconds);

    /// @brief Atomically update the maximum observed temperature
    void updateMaxTemperature(double temp) noexcept;

    /// @brief Determine target memory allocation based on system available RAM
    /// @return Target bytes to allocate, or zero when platform detection fails
    [[nodiscard]] size_t determineTargetMemoryBytes() const;

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
    [[nodiscard]] bool childrenShouldStop() const noexcept {
        return m_stop_children.load(std::memory_order_acquire) || stopRequested();
    }

    std::atomic<int> m_error_count{0};
    std::atomic<double> m_current_temp{0.0};
    std::atomic<double> m_max_temp{0.0};
    QElapsedTimer m_elapsed_timer;
};

}  // namespace sak
