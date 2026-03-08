// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_types.h
/// @brief Shared data types for the Diagnostic & Benchmarking Panel
/// @note All structs are value types suitable for signal/slot transport via Qt's meta-object system

#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>

namespace sak {

// ============================================================================
// Hardware Inventory Types
// ============================================================================

/// @brief Partition information for a storage device
struct PartitionInfo {
    QString drive_letter;          ///< "C:" or empty for non-mounted
    QString label;                 ///< Volume label
    QString file_system;           ///< "NTFS" / "FAT32" / "exFAT"
    uint64_t total_bytes{0};       ///< Total partition size
    uint64_t free_bytes{0};        ///< Free space
    bool is_boot{false};           ///< Active/boot partition
};

/// @brief CPU information from WMI Win32_Processor
struct CpuInfo {
    QString name;                  ///< "Intel Core i7-13700K"
    QString manufacturer;          ///< "Intel" / "AMD"
    uint32_t cores{0};             ///< Physical cores
    uint32_t threads{0};           ///< Logical processors
    uint32_t base_clock_mhz{0};    ///< Base frequency
    uint32_t max_clock_mhz{0};     ///< Max turbo frequency
    uint32_t l2_cache_kb{0};       ///< L2 cache size
    uint32_t l3_cache_kb{0};       ///< L3 cache size
    QString socket;                ///< "LGA1700"
    QString architecture;          ///< "x64"
    double cpu_usage_percent{0.0}; ///< Current utilization
};

/// @brief Individual memory module from WMI Win32_PhysicalMemory
struct MemoryModuleInfo {
    QString manufacturer;          ///< "Samsung"
    QString part_number;           ///< "M471A1K43DB1-CWE"
    uint64_t capacity_bytes{0};    ///< Per-module capacity
    uint32_t speed_mhz{0};         ///< 3200
    QString memory_type;           ///< "DDR4" / "DDR5"
    QString form_factor;           ///< "DIMM" / "SODIMM"
    uint32_t slot{0};              ///< Physical slot number
    QString serial_number;
};

/// @brief Aggregate memory information
struct MemorySummary {
    uint64_t total_bytes{0};       ///< Total installed RAM
    uint64_t available_bytes{0};   ///< Currently available
    uint32_t slots_used{0};
    uint32_t slots_total{0};
    QVector<MemoryModuleInfo> modules;
};

/// @brief Storage device information
struct StorageDeviceInfo {
    QString model;                 ///< "Samsung SSD 980 PRO 1TB"
    QString serial_number;
    uint64_t size_bytes{0};        ///< Total capacity
    QString interface_type;        ///< "NVMe" / "SATA" / "USB"
    QString media_type;            ///< "SSD" / "HDD" / "Unknown"
    QString firmware_version;
    uint32_t disk_number{0};       ///< Windows disk number
    double temperature{0.0};       ///< Current temp (if available)
    QVector<PartitionInfo> partitions;
};

/// @brief GPU information from DXGI + WMI
struct GpuInfo {
    QString name;                  ///< "NVIDIA GeForce RTX 4090"
    QString manufacturer;          ///< "NVIDIA" / "AMD" / "Intel"
    uint64_t vram_bytes{0};        ///< Dedicated VRAM
    QString driver_version;        ///< "546.33"
    QString driver_date;           ///< "2024-01-15"
    uint32_t current_res_x{0};     ///< Current horizontal resolution
    uint32_t current_res_y{0};     ///< Current vertical resolution
    uint32_t refresh_rate{0};      ///< Hz
};

/// @brief Motherboard information from WMI Win32_BaseBoard + Win32_BIOS
struct MotherboardInfo {
    QString manufacturer;          ///< "ASUS"
    QString product;               ///< "ROG STRIX Z790-E"
    QString serial_number;
    QString bios_version;
    QString bios_date;
    QString bios_manufacturer;
};

/// @brief Battery information from WMI Win32_Battery
struct BatteryInfo {
    bool present{false};
    QString name;
    uint32_t design_capacity{0};       ///< mWh (original design)
    uint32_t full_charge_capacity{0};  ///< mWh (current max)
    uint32_t current_charge{0};        ///< mWh (right now)
    double health_percent{0.0};        ///< fullCharge / designCapacity * 100
    QString status;                    ///< "Charging" / "Discharging" / "Full"
    uint32_t cycle_count{0};           ///< Charge cycles (if available)
    uint32_t estimated_minutes{0};     ///< Remaining runtime
};

/// @brief Complete hardware inventory snapshot
struct HardwareInventory {
    CpuInfo cpu;
    MemorySummary memory;
    QVector<StorageDeviceInfo> storage;
    QVector<GpuInfo> gpus;
    MotherboardInfo motherboard;
    BatteryInfo battery;

    // OS information
    QString os_name;               ///< "Windows 11 Pro"
    QString os_version;            ///< "23H2"
    QString os_build;              ///< "22631.3007"
    QString os_architecture;       ///< "64-bit"
    uint64_t uptime_seconds{0};    ///< System uptime

    QDateTime scan_timestamp;
};

// ============================================================================
// SMART Types
// ============================================================================

/// @brief Single SMART attribute with health assessment
struct SmartAttribute {
    uint8_t id{0};
    QString name;                  ///< "Reallocated_Sector_Ct"
    int64_t raw_value{0};          ///< Raw value
    uint8_t current_value{0};      ///< Normalized (0-253)
    uint8_t worst_value{0};        ///< Worst-ever normalized value
    uint8_t threshold{0};          ///< Failure threshold
    QString flags;                 ///< "PO-R--"
    bool failing{false};           ///< currentValue <= threshold
};

/// @brief NVMe-specific health log data
struct NvmeHealthInfo {
    uint8_t percentage_used{0};            ///< Endurance used (0-100+)
    uint64_t data_units_read{0};           ///< 512-byte units
    uint64_t data_units_written{0};
    uint64_t power_on_hours{0};
    uint32_t unsafe_shutdowns{0};
    uint32_t media_errors{0};
    uint32_t error_log_entries{0};
    int16_t temperature{0};                ///< Celsius
    uint16_t available_spare{0};           ///< Available spare NVM (%)
    uint16_t available_spare_threshold{0};
};

/// @brief Overall SMART health status
enum class SmartHealthStatus {
    Healthy,
    Warning,
    Critical,
    Unknown
};

/// @brief Per-drive SMART report
struct SmartReport {
    QString device_path;           ///< "\\\\.\\PhysicalDrive0"
    QString model;
    QString serial_number;
    QString firmware_version;
    uint64_t size_bytes{0};
    QString interface_type;        ///< "SATA" / "NVMe"

    // Overall assessment
    SmartHealthStatus overall_health{SmartHealthStatus::Unknown};
    QString smart_status;          ///< "PASSED" / "FAILED"

    // SATA attributes
    QVector<SmartAttribute> attributes;

    // NVMe health (if NVMe drive)
    std::optional<NvmeHealthInfo> nvme_health;

    // Unified key metrics (for both SATA and NVMe)
    int64_t power_on_hours{0};
    double temperature_celsius{0.0};
    int64_t reallocated_sectors{0};    ///< SATA: attr 5
    int64_t pending_sectors{0};        ///< SATA: attr 197
    double wear_level_percent{0.0};    ///< NVMe: percentageUsed

    // Recommendations
    QStringList warnings;
    QStringList recommendations;

    QDateTime scan_timestamp;
};

// ============================================================================
// Benchmark Result Types
// ============================================================================

/// @brief CPU benchmark result with individual test results and composite scores
struct CpuBenchmarkResult {
    // Individual test times (ms)
    double prime_sieve_time_ms{0.0};
    double matrix_multiply_time_ms{0.0};
    double zlib_compression_time_ms{0.0};
    double aes_encryption_time_ms{0.0};

    // Throughput metrics
    double zlib_throughput_mbps{0.0};   ///< MB/s compressed
    double aes_throughput_mbps{0.0};    ///< MB/s encrypted
    double matrix_gflops{0.0};         ///< GFLOPS

    // Composite scores (normalized: i5-12400 = 1000)
    int single_thread_score{0};
    int multi_thread_score{0};
    double thread_scaling_efficiency{0.0};

    // System info during benchmark
    uint32_t thread_count{0};
    double avg_cpu_temp_during_test{0.0};
    bool thermal_throttle_detected{false};

    QDateTime timestamp;
};

/// @brief Disk benchmark configuration
struct DiskBenchmarkConfig {
    QString drive_path;                    ///< "C:\\" or "D:\\"
    uint64_t test_file_size_mb{1024};      ///< 1 GB test file
    int sequential_block_size_kb{1024};    ///< 1 MB blocks
    int random_block_size_kb{4};           ///< 4 KB blocks
    int sequential_passes{3};              ///< Average over 3 passes
    int random_duration_sec{30};           ///< 30 seconds per random test
    int queue_depth_low{1};
    int queue_depth_high{32};
};

/// @brief Disk benchmark result with throughput and latency data
struct DiskBenchmarkResult {
    QString drive_path;
    QString drive_model;
    QString drive_type;                ///< "NVMe SSD" / "SATA SSD" / "HDD"
    uint64_t drive_capacity_bytes{0};

    // Sequential (MB/s)
    double seq_read_mbps{0.0};
    double seq_write_mbps{0.0};

    // Random 4K QD1
    double rand_4k_read_mbps{0.0};
    double rand_4k_write_mbps{0.0};
    double rand_4k_read_iops{0.0};
    double rand_4k_write_iops{0.0};

    // Random 4K QD32
    double rand_4k_qd32_read_mbps{0.0};
    double rand_4k_qd32_write_mbps{0.0};
    double rand_4k_qd32_read_iops{0.0};
    double rand_4k_qd32_write_iops{0.0};

    // Latency (microseconds)
    double avg_read_latency_us{0.0};
    double avg_write_latency_us{0.0};
    double p99_read_latency_us{0.0};
    double p99_write_latency_us{0.0};

    // Composite score (Samsung 980 PRO = 1000)
    int overall_score{0};

    QDateTime timestamp;
};

/// @brief Memory benchmark result
struct MemoryBenchmarkResult {
    // Bandwidth (GB/s)
    double read_bandwidth_gbps{0.0};
    double write_bandwidth_gbps{0.0};
    double copy_bandwidth_gbps{0.0};

    // Latency
    double random_latency_ns{0.0};

    // Allocation stress
    uint64_t max_contiguous_alloc_mb{0};
    double alloc_dealloc_ops_per_sec{0.0};

    // Composite score
    int overall_score{0};

    QDateTime timestamp;
};

// ============================================================================
// Stress Test Types
// ============================================================================

/// @brief Stress test configuration
struct StressTestConfig {
    bool stress_cpu{true};
    bool stress_memory{true};
    bool stress_disk{false};
    bool stress_gpu{false};

    int duration_minutes{10};
    int cpu_threads{0};                    ///< 0 = all logical processors
    double memory_usage_percent{80.0};     ///< Use 80% of available RAM
    QString disk_test_drive{"C:\\"};

    double thermal_limit_celsius{95.0};    ///< Auto-abort temperature
    bool abort_on_error{true};
};

/// @brief Stress test result
struct StressTestResult {
    bool passed{false};
    int duration_seconds{0};
    int errors_detected{0};

    // CPU stress results
    double avg_cpu_temp{0.0};
    double max_cpu_temp{0.0};
    int thermal_throttle_events{0};
    double avg_cpu_usage_percent{0.0};

    // Memory stress results
    uint64_t memory_bytes_written{0};
    int memory_pattern_errors{0};

    // Disk stress results
    uint64_t disk_bytes_written{0};
    int disk_errors{0};

    // GPU stress results
    uint64_t gpu_operations{0};        ///< Number of compute dispatches completed
    int gpu_errors{0};                 ///< GPU errors detected

    QString abort_reason;              ///< Empty if completed normally
    QDateTime start_time;
    QDateTime end_time;
};

// ============================================================================
// Thermal Monitoring Types
// ============================================================================

/// @brief Single thermal sensor reading
struct ThermalReading {
    QString component;             ///< "CPU Package" / "GPU" / "Disk 0"
    double temperature_celsius{0.0};
    QDateTime timestamp;
};

// ============================================================================
// Report Types
// ============================================================================

/// @brief Overall diagnostic assessment status
enum class DiagnosticStatus {
    AllPassed,
    Warnings,
    CriticalIssues
};

/// @brief Aggregated data for report generation
struct DiagnosticReportData {
    HardwareInventory inventory;
    QVector<SmartReport> smart_reports;
    std::optional<CpuBenchmarkResult> cpu_benchmark;
    std::optional<DiskBenchmarkResult> disk_benchmark;
    std::optional<MemoryBenchmarkResult> memory_benchmark;
    std::optional<StressTestResult> stress_test;

    // Overall assessment
    DiagnosticStatus overall_status{DiagnosticStatus::AllPassed};
    QStringList critical_issues;
    QStringList warnings;
    QStringList recommendations;

    // Metadata
    QString technician_name;
    QString ticket_number;
    QString notes;
    QDateTime report_timestamp;
};

} // namespace sak
