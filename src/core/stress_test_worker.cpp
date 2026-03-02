// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file stress_test_worker.cpp
/// @brief Extended stress test implementation for CPU, memory, and disk

#include "sak/stress_test_worker.h"
#include "sak/keep_awake.h"
#include "sak/logger.h"
#include "sak/layout_constants.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace sak {

namespace {

/// @brief Pattern-fill a memory region with a known repeating pattern
/// @param data Pointer to memory
/// @param size Size in bytes
/// @param seed Seed for pattern generation
void patternFill(volatile uint64_t* data, size_t count, uint64_t seed)
{
    for (size_t i = 0; i < count; ++i) {
        data[i] = seed ^ (i * 0x9E3779B97F4A7C15ULL);
    }
}

/// @brief Verify pattern integrity
/// @return Number of mismatches
int patternVerify(const volatile uint64_t* data, size_t count, uint64_t seed)
{
    int errors = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t expected = seed ^ (i * 0x9E3779B97F4A7C15ULL);
        if (data[i] != expected) {
            ++errors;
        }
    }
    return errors;
}

constexpr int kStatusIntervalSec = 5;  // Report status every 5 seconds

/// @brief Multiply a single row of a 4x4 matrix by the full matrix
void matrixRowMultiply4x4(const double mat[16], const double other[16],
                          double out[16], int row)
{
    for (int k = 0; k < 4; ++k) {
        for (int j = 0; j < 4; ++j) {
            out[row * 4 + j] += mat[row * 4 + k] * other[k * 4 + j];
        }
    }
}

/// @brief Self-multiply a 4x4 matrix in-place
void matrixSelfMultiply4x4(double mat[16])
{
    double result[16] = {};
    for (int i = 0; i < 4; ++i) {
        matrixRowMultiply4x4(mat, mat, result, i);
    }
    std::memcpy(mat, result, sizeof(double) * 16);
}

} // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

StressTestWorker::StressTestWorker(QObject* parent)
    : WorkerBase(parent)
{
}

// ============================================================================
// WorkerBase Override
// ============================================================================

auto StressTestWorker::execute() -> std::expected<void, sak::error_code>
{
    sak::KeepAwakeGuard keep_awake(sak::KeepAwake::PowerRequest::System, "Stress test");

    logInfo("Starting stress test — CPU:{} Mem:{} Disk:{} Duration:{}min",
            m_config.stress_cpu, m_config.stress_memory,
            m_config.stress_disk, m_config.duration_minutes);

    m_result = StressTestResult{};
    m_result.start_time = QDateTime::currentDateTime();
    m_error_count.store(0, std::memory_order_relaxed);
    m_max_temp.store(0.0, std::memory_order_relaxed);
    m_stop_children.store(false, std::memory_order_relaxed);

    // Launch stress threads
    std::vector<std::future<void>> futures;
    launchStressThreads(futures);

    // Monitor loop — runs in the WorkerBase thread
    const int total_seconds = m_config.duration_minutes * 60;
    monitorStressLoop(total_seconds);

    // Signal child stress threads to stop (without marking WorkerBase cancelled)
    m_stop_children.store(true, std::memory_order_release);
    for (auto& future : futures) {
        future.get();
    }

    // Finalize results
    m_result.end_time = QDateTime::currentDateTime();
    m_result.duration_seconds = static_cast<int>(m_elapsed_timer.elapsed() / 1000);
    m_result.errors_detected = m_error_count.load(std::memory_order_relaxed);
    m_result.max_cpu_temp = m_max_temp.load(std::memory_order_relaxed);
    m_result.passed = m_result.abort_reason.isEmpty() && m_result.errors_detected == 0;

    logInfo("Stress test {} — {} seconds, {} errors",
            m_result.passed ? "PASSED" : "FAILED",
            m_result.duration_seconds, m_result.errors_detected);

    Q_EMIT stressTestComplete(m_result);
    return {};
}

void StressTestWorker::launchStressThreads(std::vector<std::future<void>>& futures)
{
    if (m_config.stress_cpu) {
        const int threads = m_config.cpu_threads > 0
                                ? m_config.cpu_threads
                                : static_cast<int>(std::thread::hardware_concurrency());

        for (int thread_index = 0; thread_index < threads; ++thread_index) {
            futures.push_back(std::async(std::launch::async, [this]() {
                runCpuStress();
            }));
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
        futures.push_back(std::async(std::launch::async, [this]() {
            runDiskStress();
        }));
        logInfo("Launched disk stress thread");
    }
}

void StressTestWorker::monitorStressLoop(int total_seconds)
{
    m_elapsed_timer.start();
    int last_status_sec = 0;
    bool should_stop = false;

    while (!should_stop && m_elapsed_timer.elapsed() / 1000 < total_seconds) {
        if (checkStop()) {
            m_result.abort_reason = "Cancelled by user";
            break;
        }

        const int elapsed_sec = static_cast<int>(m_elapsed_timer.elapsed() / 1000);

        if (elapsed_sec - last_status_sec >= kStatusIntervalSec) {
            last_status_sec = elapsed_sec;
            should_stop = handleStatusUpdate(elapsed_sec, total_seconds);
        }

        // Sleep to avoid busy-waiting in monitor loop
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void StressTestWorker::updateMaxTemperature(double temp) noexcept
{
    if (temp <= 0) return;
    double prev_max = m_max_temp.load(std::memory_order_relaxed);
    while (temp > prev_max) {
        m_max_temp.compare_exchange_weak(prev_max, temp, std::memory_order_relaxed);
    }
}

bool StressTestWorker::handleStatusUpdate(int elapsed_sec, int total_seconds)
{
    const double temp = ThermalMonitor::queryCpuTemperature();
    m_current_temp.store(temp, std::memory_order_relaxed);
    updateMaxTemperature(temp);

    // Thermal abort check
    if (temp > 0 && temp >= m_config.thermal_limit_celsius) {
        logWarning("Thermal limit reached: {:.1f}°C >= {:.1f}°C — aborting",
                   temp, m_config.thermal_limit_celsius);
        m_result.abort_reason = QString("Thermal limit exceeded (%1°C)")
                                    .arg(temp, 0, 'f', 1);
        m_result.thermal_throttle_events++;
        m_stop_children.store(true, std::memory_order_release);
        return true;
    }

    const int errors = m_error_count.load(std::memory_order_relaxed);
    Q_EMIT stressTestStatus(elapsed_sec, temp, errors);

    reportProgress(elapsed_sec, total_seconds,
                   QString("Stress test running... %1/%2 sec")
                       .arg(elapsed_sec).arg(total_seconds));

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

void StressTestWorker::runCpuStress()
{
    // Sustained heavy computation: prime checking + FP work
    // This maximizes CPU utilization across all cores

    std::mt19937_64 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    while (!childrenShouldStop()) {
        // Integer workload: check if large random numbers are prime
        const uint64_t candidate = rng() | 1ULL;  // Ensure odd
        [[maybe_unused]] bool is_prime = isPrimeStress(candidate);
        if (childrenShouldStop()) return;

        // Floating-point workload: matrix operations
        alignas(64) double mat[16];
        for (int i = 0; i < 16; ++i) {
            mat[i] = static_cast<double>(rng()) / static_cast<double>(UINT64_MAX);
        }

        // 4x4 matrix self-multiply, repeated
        for (int iter = 0; iter < 100; ++iter) {
            matrixSelfMultiply4x4(mat);
        }

        // Prevent optimizer from removing everything
        volatile double sink = mat[0];
        (void)sink;
    }
}

bool StressTestWorker::isPrimeStress(uint64_t candidate) const
{
    if (candidate < 4) {
        return candidate >= 2;
    }

    const uint64_t limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(candidate)));
    for (uint64_t d = 3; d <= limit; d += 2) {
        if (candidate % d == 0) return false;
        // Periodic cancellation check every 65536 iterations
        if ((d & 0xFFFF) == 1 && childrenShouldStop()) return false;
    }
    return true;
}

// ============================================================================
// Memory Stress
// ============================================================================

int StressTestWorker::runMemoryStress()
{
    constexpr size_t kFallbackMemoryBytes = 512ULL * 1024 * 1024; // 512 MB
    const size_t target_bytes = determineTargetMemoryBytes(kFallbackMemoryBytes);

    // Cap at available memory, minimum 64 MB, maximum 16 GB
    constexpr size_t kMaxAlloc = 16ULL * 1024 * 1024 * 1024;
    const size_t alloc_size = std::clamp(target_bytes,
                                         static_cast<size_t>(64 * sak::kBytesPerMB),
                                         kMaxAlloc);

    logInfo("Memory stress: allocating {} MB", alloc_size / sak::kBytesPerMB);

    auto* data = allocateStressMemory(alloc_size);
    if (!data) {
        logError("Memory stress: allocation failed");
        return 0;
    }

    const size_t count = alloc_size / sizeof(uint64_t);
    int total_errors = 0;
    uint64_t total_bytes_written = 0;
    uint64_t pattern_seed = 0xCAFEBABE;

    while (!childrenShouldStop()) {
        patternFill(data, count, pattern_seed);
        total_bytes_written += alloc_size;

        int errors = patternVerify(data, count, pattern_seed);
        if (errors > 0) {
            logError("Memory stress: {} pattern errors with seed {:#x}",
                     errors, pattern_seed);
            total_errors += errors;
        }

        ++pattern_seed;
    }

    m_result.memory_bytes_written = total_bytes_written;
    m_result.memory_pattern_errors = total_errors;

    freeStressMemory(data);

    logInfo("Memory stress: wrote {} GB, {} pattern errors",
            total_bytes_written / static_cast<uint64_t>(sak::kBytesPerGB), total_errors);
    return total_errors;
}

size_t StressTestWorker::determineTargetMemoryBytes(size_t fallback_bytes) const
{
#ifdef SAK_PLATFORM_WINDOWS
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        return static_cast<size_t>(
            static_cast<double>(mem_status.ullAvailPhys) *
            (m_config.memory_usage_percent / 100.0));
    }
    logWarning("GlobalMemoryStatusEx failed (error {}), "
               "using {} MB fallback allocation",
               GetLastError(), fallback_bytes / sak::kBytesPerMB);
    return fallback_bytes;
#else
    #if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    {
        const long pages = sysconf(_SC_AVPHYS_PAGES);
        const long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0) {
            return static_cast<size_t>(
                static_cast<double>(pages) * static_cast<double>(page_size) *
                (m_config.memory_usage_percent / 100.0));
        }
        logWarning("sysconf memory query failed, using {} MB fallback",
                   fallback_bytes / sak::kBytesPerMB);
    }
    #else
    logInfo("Platform memory detection unavailable, using {} MB fallback",
            fallback_bytes / sak::kBytesPerMB);
    #endif
    return fallback_bytes;
#endif
}

volatile uint64_t* StressTestWorker::allocateStressMemory(size_t alloc_size)
{
#ifdef SAK_PLATFORM_WINDOWS
    return static_cast<volatile uint64_t*>(
        VirtualAlloc(nullptr, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
    return static_cast<volatile uint64_t*>(std::malloc(alloc_size));
#endif
}

void StressTestWorker::freeStressMemory(volatile uint64_t* data)
{
#ifdef SAK_PLATFORM_WINDOWS
    VirtualFree(const_cast<uint64_t*>(data), 0, MEM_RELEASE);
#else
    std::free(const_cast<uint64_t*>(data));
#endif
}

// ============================================================================
// Disk Stress
// ============================================================================

void StressTestWorker::runDiskStress()
{
#ifdef SAK_PLATFORM_WINDOWS
    const QString test_path = QDir(m_config.disk_test_drive).filePath("sak_stress_test.tmp");
    const std::wstring wpath = test_path.toStdWString();

    constexpr size_t kBlockSize = 1024 * 1024; // 1 MB blocks
    constexpr size_t kFileSize = 256ULL * 1024 * 1024; // 256 MB file

    // Allocate aligned buffer
    auto* buf = static_cast<uint8_t*>(_aligned_malloc(kBlockSize, 4096));
    if (!buf) {
        logError("Disk stress: buffer allocation failed");
        return;
    }

    // Fill buffer
    std::mt19937 rng(0xD15C);
    auto* data32 = reinterpret_cast<uint32_t*>(buf);
    for (size_t i = 0; i < kBlockSize / sizeof(uint32_t); ++i) {
        data32[i] = rng();
    }

    uint64_t total_bytes_written = 0;
    int disk_errors = 0;

    while (!childrenShouldStop()) {
        HANDLE h = CreateFileW(
            wpath.c_str(),
            GENERIC_WRITE | GENERIC_READ,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
            nullptr);

        if (h == INVALID_HANDLE_VALUE) {
            ++disk_errors;
            logError("Disk stress: failed to create test file");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Write phase
        disk_errors += writeDiskStressFile(h, buf, kBlockSize, kFileSize, total_bytes_written);

        FlushFileBuffers(h);
        CloseHandle(h);
    }

    // Cleanup
    _aligned_free(buf);
    DeleteFileW(wpath.c_str());

    m_result.disk_bytes_written = total_bytes_written;
    m_result.disk_errors = disk_errors;

    logInfo("Disk stress: wrote {} GB, {} errors",
            total_bytes_written / static_cast<uint64_t>(sak::kBytesPerGB), disk_errors);
#endif
}

#ifdef SAK_PLATFORM_WINDOWS
int StressTestWorker::writeDiskStressFile(
    void* file_handle, const uint8_t* buf,
    size_t blockSize, size_t fileSize,
    uint64_t& total_bytes_written)
{
    HANDLE h = static_cast<HANDLE>(file_handle);
    for (size_t written = 0; written < fileSize && !childrenShouldStop(); written += blockSize) {
        DWORD bytes_written = 0;
        if (!WriteFile(h, buf, static_cast<DWORD>(blockSize), &bytes_written, nullptr)) {
            return 1;
        }
        total_bytes_written += bytes_written;
    }
    return 0;
}
#endif

} // namespace sak
