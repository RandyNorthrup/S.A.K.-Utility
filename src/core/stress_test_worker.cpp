// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file stress_test_worker.cpp
/// @brief Extended stress test implementation for CPU, memory, and disk

#include "sak/stress_test_worker.h"

#include "sak/keep_awake.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#endif

namespace sak {

namespace {

constexpr uint64_t kMemoryPatternMultiplier = 0x9E'37'79'B9'7F'4A'7C'15ULL;
constexpr uint64_t kFirstCompositeCandidate = 4;
constexpr uint64_t kSmallestPrimeCandidate = 2;
constexpr uint64_t kFirstOddDivisor = 3;
constexpr uint64_t kOddDivisorStep = 2;
constexpr uint64_t kPrimeCancelCheckMask = 0xFFFF;
constexpr uint64_t kPrimeCancelCheckRemainder = 1;
constexpr int kMonitorSleepMs = 500;
constexpr int kCacheLineAlignment = 64;
constexpr int kMatrixDimension = 4;
constexpr int kMatrixElementCount = kMatrixDimension * kMatrixDimension;
constexpr int kMatrixSelfMultiplyIterations = 100;
constexpr size_t kMinimumMemoryStressBytes = 64ULL * sak::kBytesPerMB;
constexpr uint64_t kInitialMemoryPatternSeed = 0xCA'FE'BA'BEULL;
constexpr size_t kDiskBufferAlignment = 4096;
constexpr uint32_t kDiskStressRandomSeed = 0xD15CU;
constexpr uint64_t kGpuHealthCheckMask = 0xFFULL;
// Upper bound on the requested run length. duration_minutes * kSecondsPerMinute is
// computed as int; capping at one week keeps total_seconds well within int range so
// the monitor-loop bound never overflows. A longer request is rejected, not clamped.
constexpr int kMaxStressDurationMinutes = 7 * 24 * 60;  // 10080 minutes
// Upper bound on an explicitly-requested CPU stress thread count. A larger request is CLAMPED (not
// rejected) so a bogus or hostile cpu_threads value cannot spawn an unbounded number of std::async
// worker threads (resource exhaustion, or a std::system_error from thread creation); 4096 is far
// above any legitimate oversubscription need on real hardware.
constexpr int kMaxCpuStressThreads = 4096;

/// @brief Pattern-fill a memory region with a known repeating pattern
/// @param data Pointer to memory
/// @param size Size in bytes
/// @param seed Seed for pattern generation
void patternFill(volatile uint64_t* data, size_t count, uint64_t seed) {
    for (size_t i = 0; i < count; ++i) {
        data[i] = seed ^ (i * kMemoryPatternMultiplier);
    }
}

/// @brief Verify pattern integrity
/// @return Number of mismatches
int patternVerify(const volatile uint64_t* data, size_t count, uint64_t seed) {
    size_t errors = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t expected = seed ^ (i * kMemoryPatternMultiplier);
        if (data[i] != expected) {
            ++errors;
        }
    }
    // A single >16 GiB pass has > INT_MAX words, so a fully-corrupt pass would
    // overflow the signed int result type (and StressTestResult::memory_pattern_errors,
    // whose type is fixed in the public header). Saturate instead of invoking UB.
    constexpr size_t kMaxInt = static_cast<size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(errors, kMaxInt));
}

constexpr int kStatusIntervalSec = 5;  // Report status every 5 seconds

/// @brief Multiply a single row of a 4x4 matrix by the full matrix
void matrixRowMultiply4x4(const double mat[kMatrixElementCount],
                          const double other[kMatrixElementCount],
                          double out[kMatrixElementCount],
                          int row) {
    for (int k = 0; k < kMatrixDimension; ++k) {
        for (int j = 0; j < kMatrixDimension; ++j) {
            out[(row * kMatrixDimension) + j] += mat[(row * kMatrixDimension) + k] *
                                                 other[(k * kMatrixDimension) + j];
        }
    }
}

/// @brief Self-multiply a 4x4 matrix in-place
void matrixSelfMultiply4x4(double mat[kMatrixElementCount]) {
    double result[kMatrixElementCount] = {};
    for (int i = 0; i < kMatrixDimension; ++i) {
        matrixRowMultiply4x4(mat, mat, result, i);
    }
    std::memcpy(mat, result, sizeof(double) * kMatrixElementCount);
}

#ifdef SAK_PLATFORM_WINDOWS
/// @brief Atomically create and pin a unique disk-stress test file for the whole run.
/// @return An open, exclusive handle to the file, or INVALID_HANDLE_VALUE on failure.
/// @note One handle for the entire run closes the claim->close->reopen and the
///       close->delete-by-name TOCTOU windows the old two-step design still left open:
///       CREATE_NEW fails closed if the name already exists (including a planted symlink,
///       which FILE_FLAG_OPEN_REPARSE_POINT stops us following), the exclusive share mode
///       (0) blocks any swap or delete while the handle is open, and
///       FILE_FLAG_DELETE_ON_CLOSE removes the file THROUGH this handle on close -- so it
///       is never resolved by name for deletion. The unique per-run name (process id +
///       timestamp) keeps a crashed prior run's leftover from blocking a new run.
HANDLE openExclusiveStressFile(const QString& drive) {
    const QString name = QString("sak_stress_test_%1_%2.tmp")
                             .arg(GetCurrentProcessId())
                             .arg(QDateTime::currentMSecsSinceEpoch());
    const std::wstring wpath = QDir(drive).filePath(name).toStdWString();
    HANDLE h = CreateFileW(wpath.c_str(),
                           GENERIC_WRITE | GENERIC_READ | DELETE,
                           0,           // exclusive: nothing can swap or delete the file mid-run
                           nullptr,
                           CREATE_NEW,  // fail closed if the name already exists
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH |
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_DELETE_ON_CLOSE,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    // Belt-and-suspenders: CREATE_NEW already guarantees a freshly created regular file,
    // but reject anything carrying a reparse attribute or more than one hard link so this
    // handle can only ever write to (and delete on close) the unique file we just created.
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || info.nNumberOfLinks != 1) {
        CloseHandle(h);  // FILE_FLAG_DELETE_ON_CLOSE removes the file we just created
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/// @brief Fill a disk-stress block buffer with reproducible pseudo-random data.
void fillDiskStressBuffer(uint8_t* buf, size_t size) {
    std::mt19937 rng(kDiskStressRandomSeed);
    auto* data32 = reinterpret_cast<uint32_t*>(buf);
    for (size_t i = 0; i < size / sizeof(uint32_t); ++i) {
        data32[i] = rng();
    }
}
#endif

// At least one stress component must be enabled. A run with everything disabled launches
// nothing yet computeStressPassed() would otherwise report PASSED -- fail closed.
std::expected<void, sak::error_code> validateComponentsEnabled(const StressTestConfig& cfg) {
    if (!cfg.stress_cpu && !cfg.stress_memory && !cfg.stress_disk && !cfg.stress_gpu) {
        return std::unexpected(sak::error_code::invalid_configuration);
    }
    return {};
}

// Reject a nonpositive or absurd duration: duration_minutes * kSecondsPerMinute is computed as
// int, and rejecting anything above kMaxStressDurationMinutes keeps total_seconds within int
// range so the monitor-loop bound never overflows (a longer request is rejected, not clamped).
std::expected<void, sak::error_code> validateDuration(const StressTestConfig& cfg) {
    if (cfg.duration_minutes <= 0 || cfg.duration_minutes > kMaxStressDurationMinutes) {
        return std::unexpected(sak::error_code::invalid_argument);
    }
    return {};
}

// Reject an invalid memory percentage so determineTargetMemoryBytes never casts a negative/NaN
// value. !(x > 0) rejects <= 0 and NaN; the upper check rejects > 100%.
std::expected<void, sak::error_code> validateMemoryPercent(const StressTestConfig& cfg) {
    if (cfg.stress_memory &&
        (!(cfg.memory_usage_percent > 0.0) || cfg.memory_usage_percent > sak::kPercentMaxF)) {
        return std::unexpected(sak::error_code::invalid_argument);
    }
    return {};
}

// The thermal-abort check compares temp >= thermal_limit_celsius; a NaN or +Inf limit
// makes that comparison never fire, silently disabling thermal protection. Require a
// finite, positive limit and fail closed otherwise (a large FINITE limit is still
// accepted for a deliberately hot run).
std::expected<void, sak::error_code> validateThermalLimit(const StressTestConfig& cfg) {
    if (!std::isfinite(cfg.thermal_limit_celsius) || cfg.thermal_limit_celsius <= 0.0) {
        return std::unexpected(sak::error_code::invalid_argument);
    }
    return {};
}

// A disk stress target must be an explicit, absolute path. An empty or relative
// disk_test_drive would resolve against the process working directory (a guessed
// target); fail closed rather than create the stress file somewhere unintended.
std::expected<void, sak::error_code> validateDiskTarget(const StressTestConfig& cfg) {
    if (cfg.stress_disk) {
        const QString drive = cfg.disk_test_drive.trimmed();
        if (drive.isEmpty() || !QDir::isAbsolutePath(drive)) {
            return std::unexpected(sak::error_code::invalid_argument);
        }
    }
    return {};
}

// Fail closed on a stress config that would do no work, overflow the monitor timer, or feed an
// out-of-range value to a later stage. Each guard below is a separate fail-closed check; the
// order is preserved because the first failure is the one execute() logs and returns.
std::expected<void, sak::error_code> validateStressConfig(const StressTestConfig& cfg) {
    if (auto status = validateComponentsEnabled(cfg); !status) {
        return status;
    }
    if (auto status = validateDuration(cfg); !status) {
        return status;
    }
    if (auto status = validateMemoryPercent(cfg); !status) {
        return status;
    }
    if (auto status = validateThermalLimit(cfg); !status) {
        return status;
    }
    return validateDiskTarget(cfg);
}

}  // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

StressTestWorker::StressTestWorker(QObject* parent) : WorkerBase(parent) {}

// ============================================================================
// WorkerBase Override
// ============================================================================

auto StressTestWorker::execute() -> std::expected<void, sak::error_code> {
    sak::KeepAwakeGuard keep_awake(sak::KeepAwake::PowerRequest::System, "Stress test");

    // Fail closed BEFORE any work on a config that would do nothing yet pass, or whose
    // duration would overflow the monitor timer. A run that reaches the code below has
    // at least one component and a sane, non-overflowing duration.
    if (auto status = validateStressConfig(m_config); !status) {
        logError(
            "Stress test: invalid config (cpu:{} mem:{} disk:{} gpu:{} dur:{}min mem%:{:.1f})"
            " -- refusing to run",
            m_config.stress_cpu,
            m_config.stress_memory,
            m_config.stress_disk,
            m_config.stress_gpu,
            m_config.duration_minutes,
            m_config.memory_usage_percent);
        return status;
    }

    logInfo("Starting stress test -- CPU:{} Mem:{} Disk:{} GPU:{} Duration:{}min",
            m_config.stress_cpu,
            m_config.stress_memory,
            m_config.stress_disk,
            m_config.stress_gpu,
            m_config.duration_minutes);

    m_result = StressTestResult{};
    m_result.start_time = QDateTime::currentDateTime();
    m_error_count.store(0, std::memory_order_relaxed);
    m_max_temp.store(0.0, std::memory_order_relaxed);
    m_stop_children.store(false, std::memory_order_relaxed);

    if (auto status = runStressThreadsToCompletion(); !status) {
        return status;
    }

    // Finalize results
    m_result.end_time = QDateTime::currentDateTime();
    m_result.duration_seconds =
        static_cast<int>(m_elapsed_timer.elapsed() / sak::kMillisecondsPerSecond);
    m_result.errors_detected = m_error_count.load(std::memory_order_relaxed);
    m_result.max_cpu_temp = m_max_temp.load(std::memory_order_relaxed);
    m_result.passed = computeStressPassed(m_result);

    logInfo("Stress test {} -- {} seconds, {} errors",
            m_result.passed ? "PASSED" : "FAILED",
            m_result.duration_seconds,
            m_result.errors_detected);

    // The expected<void> here means "the worker RAN to completion" (matching the disk
    // benchmark and hardware workers); the PASS/FAIL verdict is m_result.passed and is
    // delivered via stressTestComplete(m_result). A failing run is NOT an execute()
    // error -- only a config/setup failure returns std::unexpected above.
    Q_EMIT stressTestComplete(m_result);
    return {};
}

std::expected<void, sak::error_code> StressTestWorker::runStressThreadsToCompletion() {
    // Launch stress threads. If launching or the monitor loop throws (e.g. std::async
    // cannot create a thread, or a vector reallocation throws), the already-running
    // children would otherwise never be told to stop -- and each std::future's destructor
    // would then block forever waiting on a child that never exits. Signal stop before
    // those destructors run and fail closed.
    std::vector<std::future<void>> futures;
    try {
        launchStressThreads(futures);

        // Monitor loop -- runs in the WorkerBase thread
        const int total_seconds = m_config.duration_minutes * sak::kSecondsPerMinute;
        monitorStressLoop(total_seconds);

        // Signal child stress threads to stop (without marking WorkerBase cancelled)
        m_stop_children.store(true, std::memory_order_release);
        for (auto& future : futures) {
            future.get();
        }
    } catch (...) {
        m_stop_children.store(true, std::memory_order_release);
        for (auto& future : futures) {
            if (future.valid()) {
                // SAK-ALLOW-BLOCKING: runs on the WorkerBase thread, not the GUI thread, and
                // the wait is bounded -- m_stop_children was just set, so each child stress
                // thread returns at its next loop check. Draining here is mandatory: a future
                // left unwaited would let a child thread outlive this scope and touch freed
                // worker state during stack unwinding.
                future.wait();
            }
        }
        logError("Stress test: aborted -- error while launching or running stress threads");
        return std::unexpected(sak::error_code::internal_error);
    }
    return {};
}

int StressTestWorker::resolveCpuThreadCount(int configThreads, unsigned int hwConcurrency) {
    if (configThreads > 0) {
        // Honor an explicit request verbatim, but clamp an absurd/hostile count so it cannot spawn
        // an unbounded number of stress threads (see kMaxCpuStressThreads).
        return std::min(configThreads, kMaxCpuStressThreads);
    }
    // configThreads <= 0 means "use all logical CPUs". hardware_concurrency()
    // returns 0 when it cannot detect the count -- fall back to one worker so a
    // requested CPU stress never launches zero threads (which would silently
    // skip the component yet still report PASSED).
    const int detected = static_cast<int>(hwConcurrency);
    return detected > 0 ? detected : 1;
}

bool StressTestWorker::computeStressPassed(const StressTestResult& result) {
    // Disk-stress failures are recorded in disk_errors but never feed errors_detected, so a total
    // disk-write failure previously still reported PASSED. Fail closed on any recorded error.
    const bool component_errors = result.disk_errors > 0 || result.memory_pattern_errors > 0 ||
                                  result.gpu_errors > 0;
    return result.abort_reason.isEmpty() && result.errors_detected == 0 && !component_errors;
}

void StressTestWorker::launchStressThreads(std::vector<std::future<void>>& futures) {
    if (m_config.stress_cpu) {
        // resolveCpuThreadCount() guarantees >= 1: hardware_concurrency() can
        // report 0, which previously launched zero threads -- a requested CPU
        // stress that silently did nothing yet still reported PASSED.
        const int threads = resolveCpuThreadCount(m_config.cpu_threads,
                                                  std::thread::hardware_concurrency());

        for (int thread_index = 0; thread_index < threads; ++thread_index) {
            futures.push_back(std::async(std::launch::async, [this]() { runCpuStress(); }));
        }
        logInfo("Launched {} CPU stress threads", threads);
    }

    if (m_config.stress_memory) {
        futures.push_back(std::async(std::launch::async, [this]() {
            int errors = runMemoryStress();
            m_error_count.fetch_add(errors, std::memory_order_relaxed);
        }));
        logInfo("Launched memory stress thread");
    }

    if (m_config.stress_disk) {
        futures.push_back(std::async(std::launch::async, [this]() { runDiskStress(); }));
        logInfo("Launched disk stress thread");
    }

    if (m_config.stress_gpu) {
        futures.push_back(std::async(std::launch::async, [this]() { runGpuStress(); }));
        logInfo("Launched GPU stress thread");
    }
}

void StressTestWorker::monitorStressLoop(int total_seconds) {
    // Invariant: execute() is this private helper's only caller, and validateStressConfig
    // already rejected duration_minutes <= 0 (and anything above kMaxStressDurationMinutes)
    // before the multiply that produces total_seconds.
    Q_ASSERT(total_seconds >= 0);
    m_elapsed_timer.start();
    int last_status_sec = 0;
    bool should_stop = false;

    while (!should_stop &&
           m_elapsed_timer.elapsed() / sak::kMillisecondsPerSecond < total_seconds) {
        if (checkStop()) {
            m_result.abort_reason = "Cancelled by user";
            break;
        }

        const int elapsed_sec =
            static_cast<int>(m_elapsed_timer.elapsed() / sak::kMillisecondsPerSecond);

        if (elapsed_sec - last_status_sec >= kStatusIntervalSec) {
            last_status_sec = elapsed_sec;
            should_stop = handleStatusUpdate(elapsed_sec, total_seconds);
        }

        // Sleep to avoid busy-waiting in monitor loop
        std::this_thread::sleep_for(std::chrono::milliseconds(kMonitorSleepMs));
    }
}

void StressTestWorker::updateMaxTemperature(double temp) noexcept {
    if (temp <= 0) {
        return;
    }
    double prev_max = m_max_temp.load(std::memory_order_relaxed);
    while (temp > prev_max) {
        m_max_temp.compare_exchange_weak(prev_max, temp, std::memory_order_relaxed);
    }
}

bool StressTestWorker::handleStatusUpdate(int elapsed_sec, int total_seconds) {
    // Invariant: monitorStressLoop is this private helper's only caller. elapsed_sec comes
    // from QElapsedTimer::elapsed() on a started timer (monotonic, never negative), and
    // total_seconds is passed straight through from the validated config (see above).
    Q_ASSERT(elapsed_sec >= 0);
    Q_ASSERT(total_seconds >= 0);
    const double temp = ThermalMonitor::queryCpuTemperature();
    m_current_temp.store(temp, std::memory_order_relaxed);
    updateMaxTemperature(temp);

    // Thermal abort check. temp <= 0 (or NaN, for which `temp > 0` is false) means the
    // sensor is unavailable -- common without admin/OEM drivers. We deliberately do NOT
    // abort in that case: with no reading there is nothing to protect against, and
    // failing closed would make stress testing impossible on every machine that cannot
    // report CPU temperature. This is an intentional accommodation, not a fail-open bug.
    if (temp > 0 && temp >= m_config.thermal_limit_celsius) {
        logWarning("Thermal limit reached: {:.1f} degC >= {:.1f} degC -- aborting",
                   temp,
                   m_config.thermal_limit_celsius);
        m_result.abort_reason = QString("Thermal limit exceeded (%1 degC)").arg(temp, 0, 'f', 1);
        m_result.thermal_throttle_events++;
        m_stop_children.store(true, std::memory_order_release);
        return true;
    }

    const int errors = m_error_count.load(std::memory_order_relaxed);
    Q_EMIT stressTestStatus(elapsed_sec, temp, errors);

    reportProgress(elapsed_sec,
                   total_seconds,
                   QString("Stress test running... %1/%2 sec").arg(elapsed_sec).arg(total_seconds));

    // Error abort check
    if (m_config.abort_on_error && errors > 0) {
        logError("Stress test aborting: {} error(s) detected", errors);
        m_result.abort_reason = QString("%1 error(s) detected").arg(errors);
        m_stop_children.store(true, std::memory_order_release);
        return true;
    }

    return false;
}

// ============================================================================
// CPU Stress
// ============================================================================

void StressTestWorker::runCpuStress() {
    // Sustained heavy computation: prime checking + FP work
    // This maximizes CPU utilization across all cores

    std::mt19937_64 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    while (!childrenShouldStop()) {
        // Integer workload: check if large random numbers are prime
        const uint64_t candidate = rng() | 1ULL;  // Ensure odd
        [[maybe_unused]] bool is_prime = isPrimeStress(candidate);

        // Re-check after compute-heavy isPrimeStress (atomic; value may change)
        // cppcheck-suppress oppositeInnerCondition
        if (childrenShouldStop()) {
            return;
        }

        // Floating-point workload: matrix operations
        alignas(kCacheLineAlignment) double mat[kMatrixElementCount];
        for (int i = 0; i < kMatrixElementCount; ++i) {
            mat[i] = static_cast<double>(rng()) / static_cast<double>(UINT64_MAX);
        }

        // 4x4 matrix self-multiply, repeated
        for (int iter = 0; iter < kMatrixSelfMultiplyIterations; ++iter) {
            matrixSelfMultiply4x4(mat);
        }

        // Prevent optimizer from removing everything
        volatile double sink = mat[0];
        (void)sink;
    }
}

bool StressTestWorker::isPrimeStress(uint64_t candidate) const {
    if (candidate < kFirstCompositeCandidate) {
        return candidate >= kSmallestPrimeCandidate;
    }

    const uint64_t limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(candidate)));
    for (uint64_t d = kFirstOddDivisor; d <= limit; d += kOddDivisorStep) {
        if (candidate % d == 0) {
            return false;
        }
        if ((d & kPrimeCancelCheckMask) == kPrimeCancelCheckRemainder && childrenShouldStop()) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Memory Stress
// ============================================================================

int StressTestWorker::runMemoryStress() {
    // No 512 MiB guessed default: if available RAM cannot be read we fail closed
    // (count one error) rather than fabricate an allocation size (no-fallbacks rule).
    // The argument is vestigial -- the signature is fixed in the public header -- and
    // ignored; determineTargetMemoryBytes returns 0 to signal a query failure.
    const size_t target_bytes = determineTargetMemoryBytes(0);
    if (target_bytes == 0) {
        logError("Memory stress: could not read available RAM -- marking memory stress failed");
        return 1;
    }

    // Cap at available memory, minimum 64 MB, maximum 16 GB
    constexpr size_t kMaxAlloc = 16ULL * 1024 * 1024 * 1024;
    const size_t alloc_size = std::clamp(target_bytes, kMinimumMemoryStressBytes, kMaxAlloc);

    logInfo("Memory stress: allocating {} MB", alloc_size / sak::kBytesPerMB);

    auto* data = allocateStressMemory(alloc_size);
    if (!data) {
        // A requested memory stress that could not allocate did NO work: count it
        // as one error (the caller folds this into errors_detected) so a memory
        // component that never ran cannot report PASSED. Return before any
        // pattern test, so memory_pattern_errors stays a pure mismatch count.
        logError("Memory stress: allocation failed -- marking memory stress failed");
        return 1;
    }

    const size_t count = alloc_size / sizeof(uint64_t);
    // Accumulate in 64-bit: a single fully-corrupt pass saturates at INT_MAX, and summing
    // several such passes into an int would overflow (signed UB). Clamp back to INT_MAX
    // only when reporting into the int result field / int return value.
    int64_t total_errors = 0;
    uint64_t total_bytes_written = 0;
    uint64_t pattern_seed = kInitialMemoryPatternSeed;

    while (!childrenShouldStop()) {
        patternFill(data, count, pattern_seed);
        total_bytes_written += alloc_size;

        int errors = patternVerify(data, count, pattern_seed);
        if (errors > 0) {
            logError("Memory stress: {} pattern errors with seed {:#x}", errors, pattern_seed);
            total_errors += errors;
        }

        ++pattern_seed;
    }

    constexpr int64_t kMaxIntErrors = static_cast<int64_t>(std::numeric_limits<int>::max());
    const int reported_errors = static_cast<int>(std::min(total_errors, kMaxIntErrors));

    m_result.memory_bytes_written = total_bytes_written;
    m_result.memory_pattern_errors = reported_errors;

    freeStressMemory(data);

    logInfo("Memory stress: wrote {} GB, {} pattern errors",
            total_bytes_written / static_cast<uint64_t>(sak::kBytesPerGB),
            reported_errors);
    return reported_errors;
}

size_t StressTestWorker::determineTargetMemoryBytes(size_t fallback_bytes) const {
    // fallback_bytes is vestigial: the old 512 MiB guessed default was removed per the
    // no-fallbacks rule. A query failure returns 0 so the caller fails the memory
    // component closed instead of allocating a fabricated size. memory_usage_percent is
    // validated in (0, 100] by validateStressConfig(), so the cast below is well-defined.
    Q_UNUSED(fallback_bytes)
#ifdef SAK_PLATFORM_WINDOWS
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        return static_cast<size_t>(static_cast<double>(mem_status.ullAvailPhys) *
                                   (m_config.memory_usage_percent / sak::kPercentMaxF));
    }
    logError("GlobalMemoryStatusEx failed (error {}) -- failing memory stress closed",
             GetLastError());
    return 0;
#else
#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    {
        const long pages = sysconf(_SC_AVPHYS_PAGES);
        const long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0) {
            return static_cast<size_t>(static_cast<double>(pages) * static_cast<double>(page_size) *
                                       (m_config.memory_usage_percent / sak::kPercentMaxF));
        }
        logError("sysconf memory query failed -- failing memory stress closed");
    }
#else
    logError("Platform memory detection unavailable -- failing memory stress closed");
#endif
    return 0;
#endif
}

volatile uint64_t* StressTestWorker::allocateStressMemory(size_t alloc_size) {
#ifdef SAK_PLATFORM_WINDOWS
    return static_cast<volatile uint64_t*>(
        VirtualAlloc(nullptr, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
    return static_cast<volatile uint64_t*>(std::malloc(alloc_size));
#endif
}

void StressTestWorker::freeStressMemory(volatile uint64_t* data) {
#ifdef SAK_PLATFORM_WINDOWS
    VirtualFree(const_cast<uint64_t*>(data), 0, MEM_RELEASE);
#else
    std::free(const_cast<uint64_t*>(data));
#endif
}

// ============================================================================
// Disk Stress
// ============================================================================

void StressTestWorker::runDiskStress() {
#ifdef SAK_PLATFORM_WINDOWS
    // One exclusive, delete-on-close handle for the whole run (see openExclusiveStressFile):
    // there is no claim->reopen or delete-by-name window for an attacker to race.
    HANDLE h = openExclusiveStressFile(m_config.disk_test_drive);
    if (h == INVALID_HANDLE_VALUE) {
        logError("Disk stress: could not exclusively create test file -- refusing to overwrite");
        m_result.disk_errors = 1;
        m_error_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    constexpr size_t kBlockSize = 1024 * 1024;          // 1 MB blocks
    constexpr size_t kFileSize = 256ULL * 1024 * 1024;  // 256 MB file
    // A disk stress that cannot allocate its buffer did no work: record an error so
    // the run cannot report PASSED (mirrors the could-not-create-file guard above).
    auto* buf = static_cast<uint8_t*>(_aligned_malloc(kBlockSize, kDiskBufferAlignment));
    if (!buf) {
        logError("Disk stress: buffer allocation failed -- marking disk stress failed");
        m_result.disk_errors = 1;
        m_error_count.fetch_add(1, std::memory_order_relaxed);
        CloseHandle(h);  // FILE_FLAG_DELETE_ON_CLOSE removes the test file
        return;
    }
    fillDiskStressBuffer(buf, kBlockSize);

    uint64_t total_bytes_written = 0;
    int disk_errors = 0;
    while (!childrenShouldStop()) {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) {
            logError("Disk stress: failed to rewind test file");
            ++disk_errors;
            m_error_count.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        // writeDiskStressFile durably writes one pass (write + flush); feed its errors
        // into m_error_count so abort_on_error can stop mid-run (it reads only that).
        const int pass_errors =
            writeDiskStressFile(h, buf, kBlockSize, kFileSize, total_bytes_written);
        if (pass_errors > 0) {
            disk_errors += pass_errors;
            m_error_count.fetch_add(pass_errors, std::memory_order_relaxed);
            break;
        }
    }

    CloseHandle(h);  // FILE_FLAG_DELETE_ON_CLOSE removes the test file
    _aligned_free(buf);

    m_result.disk_bytes_written = total_bytes_written;
    m_result.disk_errors = disk_errors;

    logInfo("Disk stress: wrote {} GB, {} errors",
            total_bytes_written / static_cast<uint64_t>(sak::kBytesPerGB),
            disk_errors);
#endif
}

#ifdef SAK_PLATFORM_WINDOWS
int StressTestWorker::writeDiskStressFile(void* file_handle,
                                          const uint8_t* buf,
                                          size_t blockSize,
                                          size_t fileSize,
                                          uint64_t& total_bytes_written) {
    HANDLE h = static_cast<HANDLE>(file_handle);
    for (size_t written = 0; written < fileSize && !childrenShouldStop(); written += blockSize) {
        DWORD bytes_written = 0;
        // A short write (bytes_written < blockSize) with NO_BUFFERING is a real device
        // error, not partial progress -- count it as a failure rather than as success.
        if (!WriteFile(h, buf, static_cast<DWORD>(blockSize), &bytes_written, nullptr) ||
            bytes_written != static_cast<DWORD>(blockSize)) {
            return 1;
        }
        total_bytes_written += bytes_written;
    }
    // A durability/stress test must confirm the writes reached the device. An ignored
    // FlushFileBuffers failure would silently pass a flush failure as a good pass.
    if (!FlushFileBuffers(h)) {
        logError("Disk stress: FlushFileBuffers failed -- durability not guaranteed");
        return 1;
    }
    return 0;
}
#endif

// ============================================================================
// GPU Stress -- RAII context and phase helpers
// ============================================================================

#ifdef SAK_PLATFORM_WINDOWS

// Compute shader HLSL: heavy ALU loop per thread
static const char* kGpuShaderSource =
    "RWBuffer<float> buf : register(u0);\n"
    "[numthreads(256, 1, 1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
    "    float v = float(id.x) * 0.001f + 1.0f;\n"
    "    [loop] for (int i = 0; i < 4096; ++i) {\n"
    "        v = v * v - v * 0.5f + 0.1f;\n"
    "        v = abs(v) < 1e15f ? v : 1.0f;\n"
    "    }\n"
    "    buf[id.x] = v;\n"
    "}\n";

constexpr UINT kGpuNumElements = 256 * 1024;
constexpr UINT kGpuGroupsX = kGpuNumElements / 256;

using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*,
                                               D3D_DRIVER_TYPE,
                                               HMODULE,
                                               UINT,
                                               const D3D_FEATURE_LEVEL*,
                                               UINT,
                                               UINT,
                                               ID3D11Device**,
                                               D3D_FEATURE_LEVEL*,
                                               ID3D11DeviceContext**);

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID,
                                        SIZE_T,
                                        LPCSTR,
                                        const D3D_SHADER_MACRO*,
                                        ID3DInclude*,
                                        LPCSTR,
                                        LPCSTR,
                                        UINT,
                                        UINT,
                                        ID3DBlob**,
                                        ID3DBlob**);

#endif  // SAK_PLATFORM_WINDOWS

}  // namespace sak

/// RAII context holding all GPU stress resources -- defined outside
/// namespace sak so the forward declaration in the header resolves.
struct sak::GpuStressContext {
#ifdef SAK_PLATFORM_WINDOWS
    HMODULE d3d11{nullptr};
    HMODULE d3dCompiler{nullptr};
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* context{nullptr};
    ID3D11ComputeShader* computeShader{nullptr};
    ID3D11Buffer* gpuBuffer{nullptr};
    ID3D11UnorderedAccessView* uav{nullptr};

    ~GpuStressContext() {
        if (uav) {
            uav->Release();
        }
        if (gpuBuffer) {
            gpuBuffer->Release();
        }
        if (computeShader) {
            computeShader->Release();
        }
        if (context) {
            context->Release();
        }
        if (device) {
            device->Release();
        }
        if (d3dCompiler) {
            FreeLibrary(d3dCompiler);
        }
        if (d3d11) {
            FreeLibrary(d3d11);
        }
    }
#endif
};

namespace sak {

bool StressTestWorker::initGpuDevice(GpuStressContext& ctx) {
#ifdef SAK_PLATFORM_WINDOWS
    // d3d11.dll is a KnownDLL (always resolved from System32), but pin the search path
    // explicitly for defense in depth and consistency with the d3dcompiler load.
    ctx.d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ctx.d3d11) {
        logWarning("GPU stress: d3d11.dll not available -- skipping");
        return false;
    }

    auto fnCreate =
        reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(ctx.d3d11, "D3D11CreateDevice"));
    if (!fnCreate) {
        logWarning("GPU stress: D3D11CreateDevice not found");
        return false;
    }

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = fnCreate(nullptr,
                          D3D_DRIVER_TYPE_HARDWARE,
                          nullptr,
                          0,
                          &feature_level,
                          1,
                          D3D11_SDK_VERSION,
                          &ctx.device,
                          nullptr,
                          &ctx.context);

    if (FAILED(hr) || !ctx.device || !ctx.context) {
        logWarning(
            "GPU stress: failed to create D3D11 device"
            " (HRESULT={:#x})",
            static_cast<unsigned>(hr));
        return false;
    }
    return true;
#else
    Q_UNUSED(ctx)
    return false;
#endif
}

bool StressTestWorker::compileGpuShader(GpuStressContext& ctx) {
#ifdef SAK_PLATFORM_WINDOWS
    // Load from System32 ONLY. d3dcompiler_47.dll is not a KnownDLL and is not always
    // present in System32, so a plain basename LoadLibrary can fall through the default
    // search order to the app dir / CWD and load a planted copy -- dangerous when the
    // tool runs elevated. Fail closed (skip GPU stress) if it is not in System32.
    ctx.d3dCompiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ctx.d3dCompiler) {
        logWarning("GPU stress: d3dcompiler_47.dll not available in System32");
        return false;
    }

    auto fnCompile =
        reinterpret_cast<PFN_D3DCompile>(GetProcAddress(ctx.d3dCompiler, "D3DCompile"));
    if (!fnCompile) {
        logWarning("GPU stress: D3DCompile not found");
        return false;
    }

    ID3DBlob* shader_blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = fnCompile(kGpuShaderSource,
                           strlen(kGpuShaderSource),
                           "gpu_stress",
                           nullptr,
                           nullptr,
                           "CSMain",
                           "cs_5_0",
                           0,
                           0,
                           &shader_blob,
                           &error_blob);

    if (FAILED(hr)) {
        if (error_blob) {
            logError("GPU stress: shader compile failed: {}",
                     static_cast<const char*>(error_blob->GetBufferPointer()));
            error_blob->Release();
        }
        if (shader_blob) {
            shader_blob->Release();
        }
        return false;
    }
    if (error_blob) {
        error_blob->Release();
    }

    hr = ctx.device->CreateComputeShader(
        shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(), nullptr, &ctx.computeShader);
    shader_blob->Release();

    if (FAILED(hr) || !ctx.computeShader) {
        logError("GPU stress: CreateComputeShader failed");
        return false;
    }
    return true;
#else
    Q_UNUSED(ctx)
    return false;
#endif
}

bool StressTestWorker::createGpuUavBuffer(GpuStressContext& ctx) {
#ifdef SAK_PLATFORM_WINDOWS
    D3D11_BUFFER_DESC buf_desc{};
    buf_desc.ByteWidth = kGpuNumElements * sizeof(float);
    buf_desc.Usage = D3D11_USAGE_DEFAULT;
    buf_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buf_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

    HRESULT hr = ctx.device->CreateBuffer(&buf_desc, nullptr, &ctx.gpuBuffer);
    if (FAILED(hr) || !ctx.gpuBuffer) {
        logError("GPU stress: CreateBuffer failed");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = kGpuNumElements;
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;

    hr = ctx.device->CreateUnorderedAccessView(ctx.gpuBuffer, &uav_desc, &ctx.uav);
    if (FAILED(hr) || !ctx.uav) {
        logError("GPU stress: CreateUnorderedAccessView failed");
        return false;
    }
    return true;
#else
    Q_UNUSED(ctx)
    return false;
#endif
}

void StressTestWorker::runGpuDispatchLoop(GpuStressContext& ctx) {
#ifdef SAK_PLATFORM_WINDOWS
    uint64_t operations = 0;
    int gpu_errors = 0;

    ctx.context->CSSetShader(ctx.computeShader, nullptr, 0);
    ctx.context->CSSetUnorderedAccessViews(0, 1, &ctx.uav, nullptr);

    logInfo(
        "GPU stress: dispatching compute shader ({}x256"
        " threads)",
        kGpuGroupsX);

    while (!childrenShouldStop()) {
        ctx.context->Dispatch(kGpuGroupsX, 1, 1);
        ctx.context->Flush();
        ++operations;

        // Periodically check for device removal (GPU crash/reset)
        if ((operations & kGpuHealthCheckMask) == 0) {
            HRESULT hr = ctx.device->GetDeviceRemovedReason();
            if (FAILED(hr)) {
                logError(
                    "GPU stress: device removed"
                    " (HRESULT={:#x})",
                    static_cast<unsigned>(hr));
                ++gpu_errors;
                m_error_count.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    m_result.gpu_operations = operations;
    m_result.gpu_errors = gpu_errors;

    logInfo("GPU stress: {} dispatches completed, {} errors", operations, gpu_errors);
#else
    Q_UNUSED(ctx)
#endif
}

void StressTestWorker::runGpuStress() {
#ifdef SAK_PLATFORM_WINDOWS
    GpuStressContext ctx;

    // A requested GPU stress that cannot initialize (no D3D11 device, shader
    // compile/create, or UAV buffer) did no work: record a GPU error so the run
    // cannot report PASSED. Previously each init failure returned silently.
    if (!initGpuDevice(ctx) || !compileGpuShader(ctx) || !createGpuUavBuffer(ctx)) {
        logError("GPU stress: initialization failed -- marking GPU stress failed");
        m_result.gpu_errors = 1;
        // Feed into the shared counter so abort_on_error can act on a GPU-init failure
        // (handleStatusUpdate reads only m_error_count), not just the final verdict.
        m_error_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    runGpuDispatchLoop(ctx);
    // ~GpuStressContext releases all D3D11 resources
#else
    // Requested but unsupported here -> not a pass.
    logWarning("GPU stress: not supported on this platform -- marking GPU stress failed");
    m_result.gpu_errors = 1;
    m_error_count.fetch_add(1, std::memory_order_relaxed);
#endif
}

}  // namespace sak
