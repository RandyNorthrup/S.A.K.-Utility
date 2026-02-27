# Diagnostic & Benchmarking Panel - Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: February 25, 2026  
**Status**: ⏳ Planned  
**Target Release**: v1.0.0

---

## 🎯 Executive Summary

The Diagnostic & Benchmarking Panel transforms S.A.K. Utility into a full hardware diagnostic workstation, enabling technicians to rapidly assess system health, identify failing components, stress-test hardware, and generate professional reports — all from a single portable tool. This panel is essential for pre-deployment validation, RMA troubleshooting, post-repair verification, and fleet health audits.

### Key Objectives
- ✅ **Hardware Inventory** - Complete system hardware enumeration (CPU, RAM, GPU, storage, motherboard, battery)
- ✅ **SMART Disk Health** - Full S.M.A.R.T. attribute monitoring with failure prediction via bundled `smartctl`
- ✅ **CPU Benchmarking** - Multi-threaded computation benchmarks with scoring and historical comparison
- ✅ **Disk I/O Benchmarking** - Sequential/random read/write throughput and IOPS measurement
- ✅ **Memory Benchmarking** - Bandwidth, latency, and allocation stress testing
- ✅ **Stress Testing** - Extended CPU, memory, and disk stress tests with thermal monitoring
- ✅ **Thermal & Sensor Monitoring** - Real-time CPU/GPU temperature, fan speed, and voltage via WMI
- ✅ **Report Generation** - Professional HTML/PDF/JSON diagnostic reports for clients and inventory systems
- ✅ **Exportable Results** - Machine-readable JSON output for fleet management integration

---

## 📊 Project Scope

### What is Hardware Diagnostics & Benchmarking?

**Hardware Diagnostics** is the process of interrogating system components to detect faults, degradation, or misconfiguration before they cause data loss or downtime.

**Benchmarking** is the process of measuring system performance in controlled, repeatable tests to establish baselines, compare hardware, or verify repairs.

**Diagnostic Workflow**:
1. Technician opens Diagnostic & Benchmarking Panel
2. System automatically inventories all hardware (CPU, RAM, GPU, disks, battery)
3. SMART data is read from all storage devices
4. Technician selects benchmarks/stress tests to run
5. Results are scored, compared to baselines, and flagged for anomalies
6. Professional report is generated and exported

**Key Data Sources (Windows)**:
- **WMI (Windows Management Instrumentation)** - CPU, RAM, motherboard, GPU, OS info
- **DeviceIoControl / smartctl** - SMART disk attributes, NVMe health
- **SetupDi API** - Device enumeration
- **DXGI** - GPU adapter enumeration
- **GetSystemPowerStatus / WMI** - Battery health
- **MSAcpi_ThermalZoneTemperature** - CPU thermal zone (WMI)
- **Performance Counters** - CPU utilization, disk queue length, memory pressure

---

## 🎯 Use Cases

### 1. **Pre-Deployment Hardware Validation**
**Scenario**: IT procurement receives 50 new workstations; each must pass hardware QA before imaging.

**Workflow**:
1. Boot workstation, launch SAK Utility
2. Open Diagnostic & Benchmarking Panel
3. Click "Run Full Diagnostic Suite" (auto-runs all tests)
4. Tests complete in ~10 minutes: hardware inventory → SMART check → CPU benchmark → disk benchmark → memory benchmark → thermal check
5. Panel flags any component below baseline thresholds
6. Export JSON report to network share for fleet tracking
7. If all PASS → proceed to imaging; if any FAIL → RMA the unit

**Benefits**:
- Catch DOA components before imaging
- Automated pass/fail thresholds eliminate guesswork
- JSON reports feed into asset management databases

---

### 2. **Post-Repair Verification**
**Scenario**: Technician replaces a failing SSD and needs to verify the new drive and overall system health.

**Workflow**:
1. After replacement, launch SAK Utility
2. Open Diagnostic & Benchmarking Panel
3. Run SMART check on new drive → verify all attributes nominal
4. Run Disk I/O benchmark → verify new drive meets expected spec (NVMe: >3000 MB/s read)
5. Run CPU stress test for 10 minutes → verify no thermal throttling
6. Generate report → attach to service ticket

**Benefits**:
- Proves repair was successful with data/evidence
- Catches secondary issues (e.g., thermal paste dried out during SSD replacement)
- Professional report for customer confidence

---

### 3. **Slow Computer Triage**
**Scenario**: Customer complains "my computer is slow." Technician needs to identify the bottleneck.

**Workflow**:
1. Launch SAK Utility on customer PC
2. Open Diagnostic & Benchmarking Panel
3. Hardware inventory immediately shows: 4 GB RAM (Windows 11 min = 4 GB), 5400 RPM HDD
4. SMART check shows HDD has 45,000 power-on hours and reallocated sectors
5. CPU benchmark scores 40% below baseline for that CPU model
6. Disk I/O benchmark confirms 80 MB/s sequential read (vs. 500+ MB/s for SSD)
7. Technician recommends: SSD upgrade + RAM upgrade
8. Generate report for customer showing bottlenecks visually

**Benefits**:
- Data-driven diagnosis, not guesswork
- Visual report helps customers understand recommendations
- Benchmark scores provide undeniable evidence

---

### 4. **Fleet Health Audit**
**Scenario**: MSP manages 200 endpoints and needs quarterly health reports.

**Workflow**:
1. Deploy SAK Utility to managed endpoints (or run from USB during site visit)
2. Run diagnostic suite on each endpoint
3. Export JSON reports to central server
4. Aggregate reports to identify: aging drives (SMART warnings), underpowered machines, thermal issues
5. Proactively replace failing components before downtime

**Benefits**:
- Proactive maintenance reduces emergency calls
- Data-driven hardware refresh planning
- SLA compliance documentation

---

### 5. **Burn-In / Stress Testing**
**Scenario**: Custom-built workstation needs 24-hour burn-in before delivery to client.

**Workflow**:
1. Launch SAK Utility
2. Open Diagnostic & Benchmarking Panel
3. Configure stress test: CPU (100% all cores) + Memory (80% allocation pattern) + Disk (continuous I/O)
4. Set duration: 24 hours
5. Enable thermal monitoring with auto-abort if temperature exceeds 95°C
6. Leave running overnight
7. Next morning: review results — no errors, no thermal throttling, all components stable
8. Generate burn-in certificate report

**Benefits**:
- Catches intermittent hardware faults
- Thermal monitoring prevents damage
- Burn-in report serves as quality certificate

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
DiagnosticBenchmarkPanel (QWidget)
├─ DiagnosticController (QObject)
│  ├─ State: Idle / Scanning / Benchmarking / StressTesting
│  ├─ Manages: All diagnostic and benchmark workers
│  └─ Aggregates: Results, scores, recommendations
│
├─ HardwareInventoryScanner (QObject) [Worker Thread]
│  ├─ WMI Queries        → CPU, RAM, Motherboard, GPU, OS
│  ├─ SetupDi API        → Device tree enumeration
│  ├─ DXGI               → GPU adapter details
│  ├─ GetSystemPowerStatus → Battery presence / charge
│  └─ Output: HardwareInventory struct
│
├─ SmartDiskAnalyzer (QObject) [Worker Thread]
│  ├─ Invokes bundled smartctl.exe (smartmontools)
│  ├─ Parses JSON output (--json flag)
│  ├─ Supports: SATA, NVMe, USB-attached drives
│  ├─ Attributes: Reallocated Sectors, Power-On Hours, Temperature, Wear Leveling
│  └─ Output: SmartReport per drive
│
├─ CpuBenchmarkWorker (QObject) [Worker Thread]
│  ├─ Integer arithmetic benchmark (prime sieve)
│  ├─ Floating-point benchmark (matrix multiplication)
│  ├─ Compression benchmark (in-memory ZLIB)
│  ├─ Encryption benchmark (AES-256 throughput)
│  ├─ Single-thread and multi-thread scores
│  └─ Output: CpuBenchmarkResult (score, ops/sec, time)
│
├─ DiskBenchmarkWorker (QObject) [Worker Thread]
│  ├─ Sequential read/write (1 MB blocks, 1 GB test file)
│  ├─ Random 4K read/write (QD1 and QD32)
│  ├─ IOPS measurement
│  ├─ Latency measurement (avg, p99)
│  └─ Output: DiskBenchmarkResult per drive
│
├─ MemoryBenchmarkWorker (QObject) [Worker Thread]
│  ├─ Sequential memory bandwidth (read/write/copy)
│  ├─ Random access latency
│  ├─ Large allocation stress
│  └─ Output: MemoryBenchmarkResult
│
├─ StressTestWorker (QObject) [Worker Thread]
│  ├─ CPU stress: all-core 100% load (integer + FP)
│  ├─ Memory stress: allocate/write/verify patterns
│  ├─ Disk stress: continuous sequential + random I/O
│  ├─ Configurable duration (minutes/hours)
│  ├─ Thermal monitoring + auto-abort threshold
│  └─ Output: StressTestResult (duration, errors, max temp, throttle events)
│
├─ ThermalMonitor (QObject) [Worker Thread, Polling]
│  ├─ WMI MSAcpi_ThermalZoneTemperature polling
│  ├─ CPU package temperature
│  ├─ GPU temperature (via WMI Win32_VideoController or NVAPI/ADL)
│  ├─ History buffer for charting
│  └─ Signals: temperatureUpdated(component, celsius)
│
└─ DiagnosticReportGenerator (QObject)
   ├─ Aggregates all results into unified report
   ├─ Pass/Fail scoring per component
   ├─ Generates: HTML (printable), JSON (machine-readable), CSV (spreadsheet)
   └─ Recommendations engine (e.g., "SSD upgrade recommended")
```

---

## 🛠️ Technical Specifications

### Hardware Inventory Scanner

**Purpose**: Enumerate all system hardware and provide a complete inventory snapshot.

**WMI Queries**:
```cpp
// CPU Information
// WMI Class: Win32_Processor
struct CpuInfo {
    QString name;            // "Intel Core i7-13700K"
    QString manufacturer;    // "Intel" / "AMD"
    uint32_t cores;          // Physical cores
    uint32_t threads;        // Logical processors
    uint32_t baseClockMHz;   // Base frequency
    uint32_t maxClockMHz;    // Max turbo frequency
    uint32_t l2CacheKB;     // L2 cache size
    uint32_t l3CacheKB;     // L3 cache size
    QString socket;          // "LGA1700"
    QString architecture;    // "x64"
    double cpuUsagePercent;  // Current utilization
};

// Memory Information
// WMI Class: Win32_PhysicalMemory
struct MemoryModuleInfo {
    QString manufacturer;    // "Samsung"
    QString partNumber;      // "M471A1K43DB1-CWE"
    uint64_t capacityBytes;  // Per-module capacity
    uint32_t speedMHz;       // 3200
    QString memoryType;      // "DDR4" / "DDR5"
    QString formFactor;      // "DIMM" / "SODIMM"
    uint32_t slot;           // Physical slot number
    QString serialNumber;
};

struct MemorySummary {
    uint64_t totalBytes;     // Total installed RAM
    uint64_t availableBytes; // Currently available
    uint32_t slotsUsed;
    uint32_t slotsTotal;
    QVector<MemoryModuleInfo> modules;
};

// Storage Information
// WMI Classes: Win32_DiskDrive, MSFT_PhysicalDisk
struct StorageDeviceInfo {
    QString model;           // "Samsung SSD 980 PRO 1TB"
    QString serialNumber;
    uint64_t sizeBytes;      // Total capacity
    QString interfaceType;   // "NVMe" / "SATA" / "USB"
    QString mediaType;       // "SSD" / "HDD" / "Unknown"
    QString firmwareVersion;
    uint32_t diskNumber;     // Windows disk number
    double temperature;      // Current temp (if available)
    QVector<PartitionInfo> partitions;
};

// GPU Information
// WMI Class: Win32_VideoController + DXGI Enumeration
struct GpuInfo {
    QString name;            // "NVIDIA GeForce RTX 4090"
    QString manufacturer;    // "NVIDIA" / "AMD" / "Intel"
    uint64_t vramBytes;      // Dedicated VRAM
    QString driverVersion;   // "546.33"
    QString driverDate;      // "2024-01-15"
    uint32_t currentResX;    // Current resolution
    uint32_t currentResY;
    uint32_t refreshRate;    // Hz
};

// Motherboard Information
// WMI Class: Win32_BaseBoard
struct MotherboardInfo {
    QString manufacturer;    // "ASUS"
    QString product;         // "ROG STRIX Z790-E"
    QString serialNumber;
    QString biosVersion;
    QString biosDate;
    QString biosManufacturer;
};

// Battery Information
// WMI Classes: Win32_Battery, BatteryStatus
struct BatteryInfo {
    bool present;
    QString name;
    uint32_t designCapacity;    // mWh (original)
    uint32_t fullChargeCapacity; // mWh (current max)
    uint32_t currentCharge;     // mWh (right now)
    double healthPercent;       // fullCharge / designCapacity * 100
    QString status;             // "Charging" / "Discharging" / "Full"
    uint32_t cycleCount;        // Charge cycles (if available)
    uint32_t estimatedMinutes;  // Remaining runtime
};

// Complete Hardware Inventory
struct HardwareInventory {
    CpuInfo cpu;
    MemorySummary memory;
    QVector<StorageDeviceInfo> storage;
    QVector<GpuInfo> gpus;
    MotherboardInfo motherboard;
    BatteryInfo battery;
    
    // OS Info
    QString osName;          // "Windows 11 Pro"
    QString osVersion;       // "23H2"
    QString osBuild;         // "22631.3007"
    QString osArchitecture;  // "64-bit"
    uint64_t uptimeSeconds;  // System uptime
    
    QDateTime scanTimestamp;
};
```

**Hardware Inventory Scanner Implementation**:
```cpp
class HardwareInventoryScanner : public QObject {
    Q_OBJECT
public:
    explicit HardwareInventoryScanner(QObject* parent = nullptr);
    
    void scan();         // Full hardware scan (async)
    void scanCpu();      // CPU only
    void scanMemory();   // Memory only
    void scanStorage();  // Storage only
    void scanGpu();      // GPU only
    void scanBattery();  // Battery only
    
Q_SIGNALS:
    void scanStarted();
    void scanProgress(int percent, QString component); // "Scanning CPU..."
    void scanComplete(HardwareInventory inventory);
    void errorOccurred(QString error);
    
private:
    HardwareInventory m_inventory;
    
    CpuInfo queryCpu();           // WMI Win32_Processor
    MemorySummary queryMemory();  // WMI Win32_PhysicalMemory
    QVector<StorageDeviceInfo> queryStorage(); // WMI + DeviceIoControl
    QVector<GpuInfo> queryGpu();  // DXGI + WMI
    MotherboardInfo queryMotherboard(); // WMI Win32_BaseBoard + Win32_BIOS
    BatteryInfo queryBattery();   // WMI Win32_Battery
    
    // WMI helper
    QVariant wmiQuery(const QString& wmiClass, const QString& property);
    QVector<QVariantMap> wmiQueryMultiple(const QString& wmiClass, 
                                          const QStringList& properties);
};
```

**WMI Query Helper (PowerShell-based)**:
```cpp
QVector<QVariantMap> HardwareInventoryScanner::wmiQueryMultiple(
    const QString& wmiClass, const QStringList& properties) 
{
    // Use PowerShell for WMI queries (most reliable on modern Windows)
    // Output as JSON for easy parsing
    QString propList = properties.join(", ");
    QString psCommand = QString(
        "Get-CimInstance -ClassName %1 | "
        "Select-Object %2 | "
        "ConvertTo-Json -Compress"
    ).arg(wmiClass, propList);
    
    QProcess ps;
    ps.start("powershell.exe", {"-NoProfile", "-Command", psCommand});
    ps.waitForFinished(30000);
    
    QJsonDocument doc = QJsonDocument::fromJson(ps.readAllStandardOutput());
    // Parse JSON array into QVector<QVariantMap>
    // ...
}
```

---

### SMART Disk Analyzer (smartmontools)

**Purpose**: Read S.M.A.R.T. attributes from all storage devices to detect imminent failures.

**Why smartmontools?**
- **Open source** (GPLv2) — freely redistributable
- **Cross-platform** — works on Windows, Linux, macOS
- **Comprehensive** — supports SATA, NVMe, SAS, USB-attached drives
- **JSON output** — `smartctl --json` provides structured data
- **Industry standard** — used by data centers worldwide
- **Portable** — single executable, no installation required

**Bundle Strategy**:
```
tools/
└─ smartmontools/
   ├─ smartctl.exe          # SMART control utility (~1.5 MB)
   ├─ COPYING               # GPLv2 license
   └─ README.txt            # Version and attribution
```

**Bundle Script** (`scripts/bundle_smartmontools.ps1`):
```powershell
# Download from official releases
# URL: https://github.com/smartmontools/smartmontools/releases
# Verify SHA-256 hash
# Extract smartctl.exe from installer or portable zip
```

**SMART Analyzer Implementation**:
```cpp
class SmartDiskAnalyzer : public QObject {
    Q_OBJECT
public:
    // SMART attribute with health assessment
    struct SmartAttribute {
        uint8_t id;
        QString name;           // "Reallocated_Sector_Ct"
        int64_t rawValue;       // Raw value
        uint8_t currentValue;   // Normalized (0-100 or 0-253)
        uint8_t worstValue;     // Worst-ever normalized value
        uint8_t threshold;      // Failure threshold
        QString flags;          // "PO-R--" (Pre-fail, Old-age, etc.)
        bool failing;           // currentValue <= threshold
    };
    
    // NVMe-specific health info
    struct NvmeHealthInfo {
        uint8_t percentageUsed;       // Endurance used (0-100+)
        uint64_t dataUnitsRead;       // 512-byte units
        uint64_t dataUnitsWritten;
        uint64_t powerOnHours;
        uint32_t unsafeShutdowns;
        uint32_t mediaErrors;
        uint32_t errorLogEntries;
        int16_t temperature;          // Celsius
        uint16_t availableSpare;      // Available spare NVM (%)
        uint16_t availableSpareThreshold;
    };
    
    // Per-drive SMART report
    struct SmartReport {
        QString devicePath;      // "\\.\PhysicalDrive0"
        QString model;
        QString serialNumber;
        QString firmwareVersion;
        uint64_t sizeBytes;
        QString interfaceType;   // "SATA" / "NVMe"
        
        // Overall assessment
        enum HealthStatus { Healthy, Warning, Critical, Unknown };
        HealthStatus overallHealth;
        QString smartStatus;     // "PASSED" / "FAILED"
        
        // SATA attributes
        QVector<SmartAttribute> attributes;
        
        // NVMe health (if NVMe)
        std::optional<NvmeHealthInfo> nvmeHealth;
        
        // Key metrics (unified for SATA and NVMe)
        int64_t powerOnHours;
        double temperatureCelsius;
        int64_t reallocatedSectors;  // SATA: attr 5
        int64_t pendingSectors;       // SATA: attr 197
        double wearLevelPercent;      // NVMe: percentageUsed
        
        // Recommendations
        QStringList warnings;    // "High reallocated sector count"
        QStringList recommendations; // "Backup data and replace drive"
        
        QDateTime scanTimestamp;
    };
    
    explicit SmartDiskAnalyzer(QObject* parent = nullptr);
    
    void scanAllDrives();
    void scanDrive(const QString& devicePath);
    
Q_SIGNALS:
    void scanStarted();
    void scanProgress(int driveIndex, int totalDrives, QString driveName);
    void driveScanned(SmartReport report);
    void scanComplete(QVector<SmartReport> reports);
    void errorOccurred(QString error);
    
private:
    QString m_smartctlPath;  // Path to bundled smartctl.exe
    
    SmartReport parseSmartctlJson(const QJsonObject& json);
    HealthStatus assessHealth(const SmartReport& report);
    QStringList generateWarnings(const SmartReport& report);
    QStringList generateRecommendations(const SmartReport& report);
    
    // Critical SMART attributes and their thresholds
    static constexpr int ATTR_REALLOCATED_SECTORS = 5;
    static constexpr int ATTR_SPIN_RETRY_COUNT = 10;
    static constexpr int ATTR_REALLOCATED_EVENT_COUNT = 196;
    static constexpr int ATTR_CURRENT_PENDING_SECTORS = 197;
    static constexpr int ATTR_OFFLINE_UNCORRECTABLE = 198;
    static constexpr int ATTR_UDMA_CRC_ERROR_COUNT = 199;
    
    // Warning thresholds
    static constexpr int64_t WARN_REALLOCATED_SECTORS = 10;
    static constexpr int64_t CRIT_REALLOCATED_SECTORS = 100;
    static constexpr int64_t WARN_PENDING_SECTORS = 1;
    static constexpr double WARN_TEMPERATURE = 55.0;
    static constexpr double CRIT_TEMPERATURE = 70.0;
    static constexpr double WARN_NVME_WEAR = 80.0;
    static constexpr double CRIT_NVME_WEAR = 95.0;
    static constexpr int64_t WARN_POWER_ON_HOURS = 30000; // ~3.4 years
};
```

**smartctl Invocation**:
```cpp
void SmartDiskAnalyzer::scanDrive(const QString& devicePath) {
    QProcess smartctl;
    smartctl.start(m_smartctlPath, {
        "--all",           // All SMART info
        "--json=c",        // Compact JSON output
        devicePath         // e.g., "/dev/sda" or "\\.\PhysicalDrive0"
    });
    smartctl.waitForFinished(60000); // 60s timeout
    
    QJsonDocument doc = QJsonDocument::fromJson(smartctl.readAllStandardOutput());
    SmartReport report = parseSmartctlJson(doc.object());
    report.overallHealth = assessHealth(report);
    report.warnings = generateWarnings(report);
    report.recommendations = generateRecommendations(report);
    
    emit driveScanned(report);
}
```

**Example smartctl JSON Output** (parsed fields):
```json
{
  "model_name": "Samsung SSD 980 PRO 1TB",
  "serial_number": "S5GXNF0T123456",
  "firmware_version": "5B2QGXA7",
  "user_capacity": { "bytes": 1000204886016 },
  "smart_status": { "passed": true },
  "nvme_smart_health_information_log": {
    "percentage_used": 3,
    "available_spare": 100,
    "temperature": 38,
    "data_units_written": 12345678,
    "power_on_hours": 5432,
    "unsafe_shutdowns": 12,
    "media_errors": 0
  }
}
```

---

### CPU Benchmark

**Purpose**: Measure CPU computational performance with reproducible, scored tests.

**Benchmark Tests**:

| Test | Description | Measures |
|------|-------------|----------|
| Prime Sieve | Sieve of Eratosthenes to N=10,000,000 | Integer arithmetic, branch prediction |
| Matrix Multiply | 1024×1024 double-precision matrix multiply | FP throughput, cache efficiency |
| ZLIB Compression | Compress 256 MB in-memory buffer | Mixed integer + memory bandwidth |
| AES-256 | Encrypt 256 MB with AES-256-CBC | Crypto throughput (AES-NI detection) |
| Multi-thread Scale | Run all tests with 1, N/2, N threads | Thread scaling efficiency |

**Benchmark Implementation**:
```cpp
class CpuBenchmarkWorker : public QObject {
    Q_OBJECT
public:
    struct BenchmarkResult {
        // Individual test results
        double primeSieveTimeMs;
        double matrixMultiplyTimeMs;
        double zlibCompressionTimeMs;  
        double aesEncryptionTimeMs;
        
        // Throughput metrics
        double zlibThroughputMBps;     // MB/s compressed
        double aesThroughputMBps;      // MB/s encrypted
        double matrixGflops;           // GFLOPS
        
        // Scores (normalized: baseline Intel i5-12400 = 1000)
        int singleThreadScore;
        int multiThreadScore;
        double threadScalingEfficiency;  // multiScore / (singleScore * threadCount)
        
        // System info during benchmark
        uint32_t threadCount;          // Threads used for MT test
        double avgCpuTempDuringTest;   // °C (if available)
        bool thermalThrottleDetected;
        
        QDateTime timestamp;
    };
    
    explicit CpuBenchmarkWorker(QObject* parent = nullptr);
    
    void runFullBenchmark();
    void runSingleThreadBenchmark();
    void runMultiThreadBenchmark();
    
    // Cancel long-running benchmark
    void cancel();
    
Q_SIGNALS:
    void benchmarkStarted(QString testName);
    void benchmarkProgress(int percent, QString status);
    void testComplete(QString testName, double score, double timeMs);
    void benchmarkComplete(BenchmarkResult result);
    void errorOccurred(QString error);
    
private:
    std::atomic<bool> m_cancelled{false};
    
    // Individual benchmark functions
    double runPrimeSieve(int limit = 10'000'000);
    double runMatrixMultiply(int size = 1024);
    double runZlibCompression(int sizeMB = 256);
    double runAesEncryption(int sizeMB = 256);
    
    // Scoring
    int calculateScore(double timeMs, double baselineMs);
    
    // Baseline times (Intel i5-12400 @ stock, score = 1000)
    static constexpr double BASELINE_PRIME_MS = 450.0;
    static constexpr double BASELINE_MATRIX_MS = 2800.0;
    static constexpr double BASELINE_ZLIB_MS = 3200.0;
    static constexpr double BASELINE_AES_MS = 1500.0;
};
```

**Matrix Multiply Implementation** (optimized C++):
```cpp
double CpuBenchmarkWorker::runMatrixMultiply(int size) {
    // Allocate aligned memory for cache efficiency
    auto A = std::make_unique<double[]>(size * size);
    auto B = std::make_unique<double[]>(size * size);
    auto C = std::make_unique<double[]>(size * size);
    
    // Initialize with pseudo-random values (deterministic seed)
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    for (int i = 0; i < size * size; ++i) {
        A[i] = dist(rng);
        B[i] = dist(rng);
        C[i] = 0.0;
    }
    
    // Tiled matrix multiply for cache efficiency
    auto start = std::chrono::high_resolution_clock::now();
    
    constexpr int TILE = 64;
    for (int ii = 0; ii < size; ii += TILE) {
        for (int jj = 0; jj < size; jj += TILE) {
            for (int kk = 0; kk < size; kk += TILE) {
                for (int i = ii; i < std::min(ii + TILE, size); ++i) {
                    for (int k = kk; k < std::min(kk + TILE, size); ++k) {
                        double a_ik = A[i * size + k];
                        for (int j = jj; j < std::min(jj + TILE, size); ++j) {
                            C[i * size + j] += a_ik * B[k * size + j];
                        }
                    }
                }
            }
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    
    // Calculate GFLOPS: 2 * N^3 floating point operations
    double flops = 2.0 * std::pow(size, 3);
    double gflops = flops / (ms / 1000.0) / 1e9;
    
    return ms;
}
```

---

### Disk I/O Benchmark

**Purpose**: Measure storage device read/write performance (sequential and random).

**Test Matrix**:

| Test | Block Size | Queue Depth | Pattern | Metric |
|------|-----------|-------------|---------|--------|
| Sequential Read | 1 MB | 1 | Linear | MB/s |
| Sequential Write | 1 MB | 1 | Linear | MB/s |
| Random Read | 4 KB | 1 | Random | IOPS, MB/s |
| Random Write | 4 KB | 1 | Random | IOPS, MB/s |
| Random Read | 4 KB | 32 | Random (async) | IOPS, MB/s |
| Random Write | 4 KB | 32 | Random (async) | IOPS, MB/s |

**Implementation**:
```cpp
class DiskBenchmarkWorker : public QObject {
    Q_OBJECT
public:
    struct DiskBenchmarkConfig {
        QString drivePath;               // "C:\\" or "D:\\"
        uint64_t testFileSizeMB = 1024;  // 1 GB test file
        int sequentialBlockSizeKB = 1024; // 1 MB blocks
        int randomBlockSizeKB = 4;       // 4 KB blocks
        int sequentialPasses = 3;        // Average over 3 passes
        int randomDurationSec = 30;      // 30 seconds per random test
        int queueDepthLow = 1;
        int queueDepthHigh = 32;
    };
    
    struct DiskBenchmarkResult {
        QString drivePath;
        QString driveModel;
        QString driveType;               // "NVMe SSD" / "SATA SSD" / "HDD"
        uint64_t driveCapacityBytes;
        
        // Sequential (MB/s)
        double seqReadMBps;
        double seqWriteMBps;
        
        // Random 4K QD1
        double rand4kReadMBps;
        double rand4kWriteMBps;
        double rand4kReadIOPS;
        double rand4kWriteIOPS;
        
        // Random 4K QD32
        double rand4kQD32ReadMBps;
        double rand4kQD32WriteMBps;
        double rand4kQD32ReadIOPS;
        double rand4kQD32WriteIOPS;
        
        // Latency
        double avgReadLatencyUs;
        double avgWriteLatencyUs;
        double p99ReadLatencyUs;
        double p99WriteLatencyUs;
        
        // Score (normalized: Samsung 980 PRO = 1000)
        int overallScore;
        
        QDateTime timestamp;
    };
    
    explicit DiskBenchmarkWorker(QObject* parent = nullptr);
    
    void runBenchmark(const DiskBenchmarkConfig& config);
    void cancel();
    
Q_SIGNALS:
    void benchmarkStarted(QString drivePath);
    void benchmarkProgress(int percent, QString status); // "Sequential Read: 2450 MB/s"
    void testComplete(QString testName, double value, QString unit);
    void benchmarkComplete(DiskBenchmarkResult result);
    void errorOccurred(QString error);
    
private:
    std::atomic<bool> m_cancelled{false};
    
    double runSequentialRead(const QString& filePath, int blockSizeKB, int passes);
    double runSequentialWrite(const QString& filePath, int blockSizeKB, int passes);
    double runRandomRead(const QString& filePath, int blockSizeKB, int durationSec, int queueDepth);
    double runRandomWrite(const QString& filePath, int blockSizeKB, int durationSec, int queueDepth);
    
    // Direct I/O (bypass OS cache) using FILE_FLAG_NO_BUFFERING
    HANDLE openFileDirectIO(const QString& path, bool write);
};
```

**Direct I/O (Windows)**:
```cpp
HANDLE DiskBenchmarkWorker::openFileDirectIO(const QString& path, bool write) {
    DWORD access = write ? GENERIC_WRITE : GENERIC_READ;
    DWORD creation = write ? CREATE_ALWAYS : OPEN_EXISTING;
    
    return CreateFileW(
        path.toStdWString().c_str(),
        access,
        0,                            // No sharing during benchmark
        nullptr,
        creation,
        FILE_FLAG_NO_BUFFERING |      // Bypass OS file cache
        FILE_FLAG_WRITE_THROUGH |     // No write caching
        FILE_FLAG_SEQUENTIAL_SCAN,    // (for sequential tests)
        nullptr
    );
}
```

---

### Memory Benchmark

**Purpose**: Measure memory subsystem performance (bandwidth, latency).

**Implementation**:
```cpp
class MemoryBenchmarkWorker : public QObject {
    Q_OBJECT
public:
    struct MemoryBenchmarkResult {
        // Bandwidth (GB/s)
        double readBandwidthGBps;       // Sequential read
        double writeBandwidthGBps;      // Sequential write
        double copyBandwidthGBps;       // Memory copy
        
        // Latency
        double randomLatencyNs;         // Random access latency
        
        // Allocation stress
        uint64_t maxContiguousAllocMB;  // Largest single alloc
        double allocDeallocOpsPerSec;   // Malloc/free throughput
        
        // Score
        int overallScore;
        
        QDateTime timestamp;
    };
    
    explicit MemoryBenchmarkWorker(QObject* parent = nullptr);
    
    void runBenchmark();
    void cancel();
    
Q_SIGNALS:
    void benchmarkStarted();
    void benchmarkProgress(int percent, QString status);
    void benchmarkComplete(MemoryBenchmarkResult result);
    void errorOccurred(QString error);
    
private:
    std::atomic<bool> m_cancelled{false};
    
    double measureReadBandwidth(size_t sizeMB = 512);
    double measureWriteBandwidth(size_t sizeMB = 512);
    double measureCopyBandwidth(size_t sizeMB = 512);
    double measureRandomLatency(size_t sizeMB = 256, int iterations = 10'000'000);
};
```

---

### Stress Testing

**Purpose**: Run sustained workloads to detect intermittent hardware faults and thermal issues.

**Implementation**:
```cpp
class StressTestWorker : public QObject {
    Q_OBJECT
public:
    struct StressTestConfig {
        bool stressCpu = true;
        bool stressMemory = true;
        bool stressDisk = false;
        
        int durationMinutes = 10;          // Default: 10 min
        int cpuThreads = 0;               // 0 = all logical processors
        double memoryUsagePercent = 80.0;  // Use 80% of available RAM
        QString diskTestDrive = "C:\\";
        
        double thermalLimitCelsius = 95.0; // Auto-abort temperature
        bool abortOnError = true;          // Stop on first error
    };
    
    struct StressTestResult {
        bool passed;
        int durationSeconds;               // Actual runtime
        int errorsDetected;
        
        // CPU stress results
        double avgCpuTemp;
        double maxCpuTemp;
        int thermalThrottleEvents;
        double avgCpuUsagePercent;
        
        // Memory stress results
        uint64_t memoryBytesWritten;
        int memoryPatternErrors;           // Bit-flip errors detected
        
        // Disk stress results
        uint64_t diskBytesWritten;
        int diskErrors;
        
        QString abortReason;               // Empty if completed normally
        QDateTime startTime;
        QDateTime endTime;
    };
    
    explicit StressTestWorker(QObject* parent = nullptr);
    
    void startStressTest(const StressTestConfig& config);
    void stopStressTest();
    
Q_SIGNALS:
    void stressTestStarted();
    void stressTestProgress(int elapsedSec, int totalSec, double cpuTemp, 
                            double cpuUsage, int errors);
    void stressTestComplete(StressTestResult result);
    void thermalWarning(double temperature, double limit);
    void errorDetected(QString component, QString description);
    
private:
    std::atomic<bool> m_running{false};
    
    void cpuStressThread();        // Infinite FP + integer computation loop
    void memoryStressThread(size_t bytes);  // Write patterns, verify, repeat
    void diskStressThread(const QString& path); // Continuous I/O
    void monitorThread(const StressTestConfig& config); // Temp + error monitoring
};
```

**Memory Pattern Test** (detects bit-flip errors):
```cpp
void StressTestWorker::memoryStressThread(size_t bytes) {
    auto buffer = std::make_unique<uint8_t[]>(bytes);
    
    // Patterns to test: all-zeros, all-ones, alternating, walking bit
    const std::array<uint8_t, 4> patterns = {0x00, 0xFF, 0xAA, 0x55};
    
    while (m_running) {
        for (uint8_t pattern : patterns) {
            if (!m_running) break;
            
            // Write pattern
            std::memset(buffer.get(), pattern, bytes);
            
            // Verify pattern
            for (size_t i = 0; i < bytes; ++i) {
                if (buffer[i] != pattern) {
                    emit errorDetected("Memory", 
                        QString("Bit error at offset %1: expected 0x%2, got 0x%3")
                            .arg(i)
                            .arg(pattern, 2, 16, QChar('0'))
                            .arg(buffer[i], 2, 16, QChar('0')));
                }
            }
        }
    }
}
```

---

### Thermal Monitor

**Purpose**: Real-time temperature monitoring with history and alerting.

**Implementation**:
```cpp
class ThermalMonitor : public QObject {
    Q_OBJECT
public:
    struct ThermalReading {
        QString component;       // "CPU Package" / "GPU" / "Disk 0"
        double temperatureCelsius;
        QDateTime timestamp;
    };
    
    explicit ThermalMonitor(QObject* parent = nullptr);
    ~ThermalMonitor();
    
    void startMonitoring(int intervalMs = 1000);
    void stopMonitoring();
    
    QVector<ThermalReading> getCurrentReadings() const;
    QVector<ThermalReading> getHistory(const QString& component, int lastNSeconds = 300) const;
    
Q_SIGNALS:
    void temperatureUpdated(QVector<ThermalReading> readings);
    void thermalWarning(QString component, double temperature, double threshold);
    void thermalCritical(QString component, double temperature, double threshold);
    
private:
    QTimer* m_pollTimer;
    QVector<QVector<ThermalReading>> m_history; // Ring buffer per component
    
    double readCpuTemperature();   // WMI MSAcpi_ThermalZoneTemperature
    double readGpuTemperature();   // WMI or vendor API
    double readDiskTemperature(int diskIndex); // smartctl or WMI
    
    static constexpr double CPU_WARN_TEMP = 80.0;
    static constexpr double CPU_CRIT_TEMP = 95.0;
    static constexpr double GPU_WARN_TEMP = 85.0;
    static constexpr double GPU_CRIT_TEMP = 100.0;
};
```

---

### Diagnostic Report Generator

**Purpose**: Produce professional, readable reports from diagnostic and benchmark data.

**Report Formats**:

| Format | Use Case |
|--------|----------|
| **HTML** | Print-ready report for customers (styled, visual) |
| **JSON** | Machine-readable for fleet management / automation |
| **CSV** | Spreadsheet import for tracking / comparison |

**Implementation**:
```cpp
class DiagnosticReportGenerator : public QObject {
    Q_OBJECT
public:
    struct ReportData {
        HardwareInventory inventory;
        QVector<SmartDiskAnalyzer::SmartReport> smartReports;
        std::optional<CpuBenchmarkWorker::BenchmarkResult> cpuBenchmark;
        std::optional<DiskBenchmarkWorker::DiskBenchmarkResult> diskBenchmark;
        std::optional<MemoryBenchmarkWorker::MemoryBenchmarkResult> memBenchmark;
        std::optional<StressTestWorker::StressTestResult> stressTest;
        
        // Overall assessment
        enum OverallStatus { AllPassed, Warnings, CriticalIssues };
        OverallStatus overallStatus;
        QStringList criticalIssues;
        QStringList warnings;
        QStringList recommendations;
        
        // Metadata
        QString technicianName;
        QString ticketNumber;
        QString notes;
        QDateTime reportTimestamp;
    };
    
    explicit DiagnosticReportGenerator(QObject* parent = nullptr);
    
    void generateHTMLReport(const ReportData& data, const QString& outputPath);
    void generateJSONReport(const ReportData& data, const QString& outputPath);
    void generateCSVReport(const ReportData& data, const QString& outputPath);
    
Q_SIGNALS:
    void reportGenerated(QString path, QString format);
    void errorOccurred(QString error);
    
private:
    QString buildHTMLHeader(const ReportData& data);
    QString buildHTMLInventorySection(const HardwareInventory& inv);
    QString buildHTMLSmartSection(const QVector<SmartDiskAnalyzer::SmartReport>& reports);
    QString buildHTMLBenchmarkSection(const ReportData& data);
    QString buildHTMLRecommendations(const ReportData& data);
    
    QJsonObject buildJsonReport(const ReportData& data);
};
```

---

### Diagnostic Controller

**Purpose**: Orchestrate all diagnostic components and manage state.

```cpp
class DiagnosticController : public QObject {
    Q_OBJECT
public:
    enum class State {
        Idle,
        ScanningHardware,
        ScanningSmartData,
        BenchmarkingCpu,
        BenchmarkingDisk,
        BenchmarkingMemory,
        StressTesting,
        GeneratingReport
    };
    
    explicit DiagnosticController(QObject* parent = nullptr);
    ~DiagnosticController();
    
    // Run individual tests
    void scanHardware();
    void scanSmartData();
    void runCpuBenchmark();
    void runDiskBenchmark(const QString& drivePath);
    void runMemoryBenchmark();
    void startStressTest(const StressTestWorker::StressTestConfig& config);
    void stopStressTest();
    
    // Run full diagnostic suite (all tests in sequence)
    void runFullDiagnosticSuite();
    
    // Report generation
    void generateReport(const QString& outputPath, const QString& format);
    
    // Cancel current operation
    void cancel();
    
    // State access
    State currentState() const;
    
Q_SIGNALS:
    void stateChanged(State newState);
    void progressUpdated(int percent, QString status);
    void statusMessage(QString message, int timeout);
    
    void hardwareScanComplete(HardwareInventory inventory);
    void smartScanComplete(QVector<SmartDiskAnalyzer::SmartReport> reports);
    void cpuBenchmarkComplete(CpuBenchmarkWorker::BenchmarkResult result);
    void diskBenchmarkComplete(DiskBenchmarkWorker::DiskBenchmarkResult result);
    void memoryBenchmarkComplete(MemoryBenchmarkWorker::MemoryBenchmarkResult result);
    void stressTestComplete(StressTestWorker::StressTestResult result);
    void reportGenerated(QString path);
    
    void errorOccurred(QString error);
    
private:
    State m_state = State::Idle;
    QThread* m_workerThread;
    
    std::unique_ptr<HardwareInventoryScanner> m_hwScanner;
    std::unique_ptr<SmartDiskAnalyzer> m_smartAnalyzer;
    std::unique_ptr<CpuBenchmarkWorker> m_cpuBenchmark;
    std::unique_ptr<DiskBenchmarkWorker> m_diskBenchmark;
    std::unique_ptr<MemoryBenchmarkWorker> m_memBenchmark;
    std::unique_ptr<StressTestWorker> m_stressTest;
    std::unique_ptr<ThermalMonitor> m_thermalMonitor;
    std::unique_ptr<DiagnosticReportGenerator> m_reportGenerator;
    
    // Accumulated results for report
    DiagnosticReportGenerator::ReportData m_reportData;
};
```

---

## 🎨 User Interface Design

### Diagnostic & Benchmarking Panel Layout

```
┌─────────────────────────────────────────────────────────────────────┐
│ Diagnostic & Benchmarking Panel                                     │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────── 🖥️ HARDWARE INVENTORY ──────────────────────┐  │
│  │                                                               │  │
│  │  CPU:          Intel Core i7-13700K (16C/24T) @ 5.4 GHz     │  │
│  │  Memory:       32 GB DDR5-5600 (2/4 slots)                  │  │
│  │  GPU:          NVIDIA GeForce RTX 4070 Ti (12 GB)           │  │
│  │  Motherboard:  ASUS ROG STRIX Z790-E (BIOS: 2024-01)       │  │
│  │  OS:           Windows 11 Pro 23H2 (Build 22631)            │  │
│  │  Uptime:       3 days, 14 hours                             │  │
│  │                                                               │  │
│  │  [🔄 Rescan Hardware]  [📋 Copy to Clipboard]                │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────── 💾 STORAGE HEALTH (S.M.A.R.T.) ─────────────┐  │
│  │                                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │ Drive          | Type   | Health  | Temp | Hours  | Wear│ │  │
│  │  │────────────────────────────────────────────────────────│ │  │
│  │  │ Samsung 980 PRO| NVMe   | 🟢 PASS | 38°C | 5,432  | 3% │ │  │
│  │  │ WD Blue 2TB    | SATA   | 🟡 WARN | 42°C | 31,205 | –  │ │  │
│  │  │ Seagate 4TB    | SATA   | 🔴 FAIL | 48°C | 52,110 | –  │ │  │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  │                                                               │  │
│  │  ⚠️ WD Blue 2TB: 15 reallocated sectors detected             │  │
│  │  🔴 Seagate 4TB: 142 pending sectors — BACKUP IMMEDIATELY   │  │
│  │                                                               │  │
│  │  [🔄 Rescan SMART]  [📄 Full SMART Report]                   │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────── 📊 BENCHMARKS ───────────────────────────────┐  │
│  │                                                               │  │
│  │  ┌─ CPU Benchmark ──────────────────────────────────────┐   │  │
│  │  │ Single-Thread:  1,423  │  Multi-Thread:  14,850      │   │  │
│  │  │ ████████████████░░░░ 142% of baseline (i5-12400)     │   │  │
│  │  │ [▶️ Run CPU Benchmark]                                │   │  │
│  │  └───────────────────────────────────────────────────────┘   │  │
│  │                                                               │  │
│  │  ┌─ Disk I/O Benchmark ─────────────────────────────────┐   │  │
│  │  │ Drive: [Samsung 980 PRO (C:\)] ▼                     │   │  │
│  │  │                                                       │   │  │
│  │  │ Sequential Read:  6,842 MB/s   Write: 5,123 MB/s    │   │  │
│  │  │ Random 4K QD1:    72 MB/s (18,432 IOPS)             │   │  │
│  │  │ Random 4K QD32:   625 MB/s (160,256 IOPS)           │   │  │
│  │  │ Avg Latency:      Read: 54 µs  Write: 23 µs         │   │  │
│  │  │ [▶️ Run Disk Benchmark]                               │   │  │
│  │  └───────────────────────────────────────────────────────┘   │  │
│  │                                                               │  │
│  │  ┌─ Memory Benchmark ───────────────────────────────────┐   │  │
│  │  │ Read: 52.3 GB/s  Write: 48.1 GB/s  Copy: 45.6 GB/s  │   │  │
│  │  │ Random Latency: 68 ns                                │   │  │
│  │  │ [▶️ Run Memory Benchmark]                             │   │  │
│  │  └───────────────────────────────────────────────────────┘   │  │
│  │                                                               │  │
│  │  [▶️ Run All Benchmarks]                                      │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────── 🔥 STRESS TESTING ──────────────────────────┐  │
│  │                                                               │  │
│  │  Components:  ☑ CPU  ☑ Memory  ☐ Disk                       │  │
│  │  Duration:    [10] minutes  ▼                                │  │
│  │  Thermal Limit: [95] °C (auto-abort)                        │  │
│  │                                                               │  │
│  │  Status: ⚫ Not Running                                      │  │
│  │                                                               │  │
│  │  [▶️ Start Stress Test]                                       │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────── 🌡️ THERMAL MONITOR ─────────────────────────┐  │
│  │                                                               │  │
│  │  CPU Package: 42°C  ████░░░░░░░░░░░░░░░░  (42/95°C)       │  │
│  │  GPU:         38°C  ███░░░░░░░░░░░░░░░░░░  (38/100°C)      │  │
│  │  Disk 0:      38°C  ███░░░░░░░░░░░░░░░░░░  (38/70°C)       │  │
│  │                                                               │  │
│  │  [Temperature graph showing last 5 minutes]                   │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────── 📄 REPORT ──────────────────────────────────┐  │
│  │                                                               │  │
│  │  Technician: [________________]  Ticket #: [___________]    │  │
│  │  Notes:      [________________________________]             │  │
│  │                                                               │  │
│  │  [📄 Generate HTML Report]  [📊 Export JSON]  [📋 Export CSV] │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Stress Test Running State

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 🔥 STRESS TESTING ──────────────────────────┐  │
│  │                                                               │  │
│  │  Status: 🟢 Running (CPU + Memory)                           │  │
│  │  Elapsed: 4:32 / 10:00                                       │  │
│  │  Progress: ████████████░░░░░░░░░░  45%                      │  │
│  │                                                               │  │
│  │  CPU Usage:     100% (all 24 threads)                        │  │
│  │  CPU Temp:      87°C  ██████████████████░░  (87/95°C)       │  │
│  │  Memory Used:   25.6 GB / 32 GB (80%)                       │  │
│  │  Errors Found:  0                                             │  │
│  │  Throttle Events: 0                                          │  │
│  │                                                               │  │
│  │  [⏹️ STOP STRESS TEST]                                       │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

### Full Diagnostic Suite Running

```
┌─────────────────────────────────────────────────────────────────────┐
│  ┌──────────────── 🔄 FULL DIAGNOSTIC SUITE ───────────────────┐  │
│  │                                                               │  │
│  │  ✅ Hardware Inventory ............. Complete (2s)            │  │
│  │  ✅ SMART Disk Health .............. Complete (5s)            │  │
│  │  ✅ CPU Benchmark .................. Complete (45s)           │  │
│  │  🔄 Disk I/O Benchmark ............ Running (32%)            │  │
│  │  ⏳ Memory Benchmark .............. Pending                   │  │
│  │  ⏳ Stress Test (10 min) .......... Pending                   │  │
│  │  ⏳ Generate Report ............... Pending                   │  │
│  │                                                               │  │
│  │  Overall Progress: ████████████░░░░░░░░░░  42%              │  │
│  │  Estimated Time Remaining: 12 minutes                        │  │
│  │                                                               │  │
│  │  [⏹️ Cancel Suite]  [⏭️ Skip Current Test]                    │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
```

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
diagnostic_benchmark_panel.h          # Main UI panel
diagnostic_controller.h               # Orchestrates all diagnostic workers
hardware_inventory_scanner.h           # WMI/API hardware enumeration
smart_disk_analyzer.h                  # SMART data via smartctl
cpu_benchmark_worker.h                 # CPU benchmark tests
disk_benchmark_worker.h                # Disk I/O benchmark tests
memory_benchmark_worker.h              # Memory bandwidth/latency tests
stress_test_worker.h                   # Extended stress testing
thermal_monitor.h                      # Real-time temperature monitoring
diagnostic_report_generator.h          # HTML/JSON/CSV report generation
diagnostic_types.h                     # Shared types (HardwareInventory, etc.)
```

#### Implementation (`src/`)

```
gui/diagnostic_benchmark_panel.cpp
core/diagnostic_controller.cpp
core/hardware_inventory_scanner.cpp
core/smart_disk_analyzer.cpp
core/cpu_benchmark_worker.cpp
core/disk_benchmark_worker.cpp
core/memory_benchmark_worker.cpp
core/stress_test_worker.cpp
core/thermal_monitor.cpp
core/diagnostic_report_generator.cpp
```

#### Bundle Script

```
scripts/bundle_smartmontools.ps1       # Download + verify smartctl.exe
```

#### Resources

```
tools/smartmontools/
├─ smartctl.exe                        # SMART control utility (~1.5 MB)
├─ COPYING                            # GPLv2 license
└─ README.txt                         # Version and attribution

resources/diagnostic/
├─ report_template.html               # HTML report template
└─ icons/
   ├─ diagnostic_panel.svg
   ├─ cpu_benchmark.svg
   ├─ disk_health.svg
   └─ stress_test.svg
```

---

## 🔧 Third-Party Dependencies

### smartmontools (smartctl.exe)

| Property | Value |
|----------|-------|
| **Tool** | smartmontools (smartctl) |
| **Version** | 7.4 (latest stable) |
| **License** | GPLv2 |
| **Source** | https://www.smartmontools.org/ |
| **Download** | https://github.com/smartmontools/smartmontools/releases |
| **Size** | ~1.5 MB (smartctl.exe only) |
| **Redistributable** | ✅ Yes (GPLv2 — must include license) |
| **Purpose** | Read S.M.A.R.T. attributes from SATA, NVMe, SAS, USB drives |
| **Why this tool** | Industry standard, JSON output, portable, no installation required |
| **Alternative considered** | DeviceIoControl direct — rejected due to complexity and incomplete NVMe support |

**Bundle Script** (`scripts/bundle_smartmontools.ps1`):
```powershell
<#
.SYNOPSIS
    Downloads and bundles smartmontools (smartctl.exe) for S.A.K. Utility.
.DESCRIPTION
    Downloads the official smartmontools Windows installer from GitHub releases,
    extracts smartctl.exe, and places it in tools/smartmontools/.
    Verifies SHA-256 hash for integrity.
#>

param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$ToolName = "smartmontools"
$Version = "7.4"
$DestDir = Join-Path $PSScriptRoot "..\tools\smartmontools"
$TempDir = Join-Path $env:TEMP "sak_bundle_smartmontools"

# Download URL (official GitHub release — Windows 64-bit portable)
$DownloadUrl = "https://github.com/smartmontools/smartmontools/releases/download/RELEASE_7_4/smartmontools-7.4-1.win32-setup.exe"
$ExpectedHash = "<SHA256_HASH_OF_INSTALLER>"  # Verify from release page

# Check if already present
if ((Test-Path "$DestDir\smartctl.exe") -and -not $Force) {
    $existingVersion = & "$DestDir\smartctl.exe" --version 2>&1 | Select-String "smartctl" | Select-Object -First 1
    Write-Host "smartctl already bundled: $existingVersion"
    Write-Host "Use -Force to re-download."
    return
}

# Create directories
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

try {
    # Download installer
    $installerPath = Join-Path $TempDir "smartmontools-setup.exe"
    Write-Host "Downloading smartmontools v$Version..."
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $installerPath -UseBasicParsing

    # Verify hash
    $actualHash = (Get-FileHash -Path $installerPath -Algorithm SHA256).Hash
    if ($actualHash -ne $ExpectedHash) {
        throw "SHA-256 mismatch! Expected: $ExpectedHash, Got: $actualHash"
    }
    Write-Host "SHA-256 verified: $actualHash"

    # Extract using 7-Zip or expand the installer silently
    # smartmontools installer supports /S /D=<dir> for silent install
    $extractDir = Join-Path $TempDir "extracted"
    & $installerPath /S /D=$extractDir
    Start-Sleep -Seconds 5  # Wait for extraction

    # Copy smartctl.exe
    $smartctlSrc = Join-Path $extractDir "bin\smartctl.exe"
    if (Test-Path $smartctlSrc) {
        Copy-Item $smartctlSrc "$DestDir\smartctl.exe" -Force
    } else {
        throw "smartctl.exe not found after extraction"
    }

    # Copy license
    $licenseSrc = Join-Path $extractDir "doc\COPYING"
    if (Test-Path $licenseSrc) {
        Copy-Item $licenseSrc "$DestDir\COPYING" -Force
    }

    # Create README
    @"
smartmontools v$Version
=======================
Bundled for S.A.K. Utility diagnostic panel.
License: GPLv2 (see COPYING)
Source: https://www.smartmontools.org/
"@ | Set-Content "$DestDir\README.txt"

    # Verify
    $version = & "$DestDir\smartctl.exe" --version 2>&1 | Select-String "smartctl" | Select-Object -First 1
    Write-Host "Successfully bundled: $version"

} finally {
    # Cleanup
    if (Test-Path $TempDir) {
        Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
    }
}
```

### Native Dependencies (No Third-Party)

All other diagnostic capabilities use Windows built-in APIs:

| Capability | API / Method |
|------------|-------------|
| CPU info | WMI `Win32_Processor` via PowerShell |
| Memory info | WMI `Win32_PhysicalMemory` + `GlobalMemoryStatusEx` |
| GPU info | DXGI `IDXGIFactory::EnumAdapters` + WMI `Win32_VideoController` |
| Motherboard info | WMI `Win32_BaseBoard` + `Win32_BIOS` |
| Battery info | WMI `Win32_Battery` + `GetSystemPowerStatus` API |
| CPU temperature | WMI `MSAcpi_ThermalZoneTemperature` (requires admin) |
| CPU benchmark | Pure C++ computation (bundled ZLIB for compression test) |
| Disk benchmark | `CreateFileW` with `FILE_FLAG_NO_BUFFERING` |
| Memory benchmark | `VirtualAlloc` + pointer chase for latency |
| Performance counters | `Pdh` (Performance Data Helper) API |

---

## 🔧 Implementation Phases

### Phase 1: Hardware Inventory Scanner (Week 1-2)

**Goals**:
- Complete hardware enumeration via WMI and Windows API
- Structured output for all component categories

**Tasks**:
1. Implement `HardwareInventoryScanner` with WMI queries
2. CPU: model, cores, threads, frequencies, cache, socket
3. Memory: modules, capacity, speed, type, slots
4. Storage: drives, capacity, type, interface, partitions
5. GPU: model, VRAM, driver, resolution
6. Motherboard: manufacturer, model, BIOS version
7. Battery: health, capacity, cycle count
8. OS: version, build, architecture, uptime
9. Write unit tests with mock WMI data

**Acceptance Criteria**:
- ✅ All hardware categories enumerated
- ✅ JSON output structure matches spec
- ✅ Handles missing components gracefully (e.g., no battery on desktop)
- ✅ Completes scan in < 5 seconds

---

### Phase 2: SMART Disk Analyzer (Week 3-4)

**Goals**:
- Bundle smartctl.exe with SHA-256 verification
- Parse SMART data for SATA and NVMe drives
- Health assessment with warning/critical thresholds

**Tasks**:
1. Create `scripts/bundle_smartmontools.ps1`
2. Download and verify smartctl.exe
3. Implement `SmartDiskAnalyzer` class
4. Parse smartctl JSON output for SATA attributes
5. Parse smartctl JSON output for NVMe health log
6. Implement health assessment logic (thresholds for key attributes)
7. Generate warnings and recommendations
8. Handle USB-attached drives and edge cases
9. Write unit tests with sample smartctl output
10. Add to `THIRD_PARTY_LICENSES.md`

**Acceptance Criteria**:
- ✅ smartctl.exe bundled and verified
- ✅ SATA drives: all SMART attributes parsed
- ✅ NVMe drives: health log parsed
- ✅ Health status correctly assessed (Healthy/Warning/Critical)
- ✅ Warnings generated for degraded drives
- ✅ Bundle script reproducible

---

### Phase 3: CPU Benchmark (Week 5-6)

**Goals**:
- Implement reproducible CPU benchmarks with scoring
- Single-thread and multi-thread tests

**Tasks**:
1. Implement `CpuBenchmarkWorker` class
2. Prime sieve benchmark (integer performance)
3. Matrix multiply benchmark (floating-point performance)
4. ZLIB compression benchmark (mixed workload)
5. AES encryption benchmark (crypto throughput)
6. Multi-thread scaling test (1, N/2, N threads)
7. Scoring system normalized to baseline CPU
8. Progress reporting and cancellation support
9. Write unit tests

**Acceptance Criteria**:
- ✅ All 4 benchmark tests implemented
- ✅ Scores reproducible (< 3% variance between runs)
- ✅ Multi-thread scaling measured accurately
- ✅ Cancellation works mid-benchmark
- ✅ Results include both raw times and scores

---

### Phase 4: Disk I/O Benchmark (Week 7-8)

**Goals**:
- Measure sequential and random I/O performance
- Direct I/O to bypass OS cache for accurate results

**Tasks**:
1. Implement `DiskBenchmarkWorker` class
2. Sequential read/write with 1 MB blocks
3. Random 4K read/write at QD1 and QD32
4. IOPS calculation and latency measurement (avg, p99)
5. Direct I/O using `FILE_FLAG_NO_BUFFERING`
6. Test file creation and cleanup
7. Drive selection (test any mounted volume)
8. Scoring normalized to reference SSD
9. Write unit tests

**Acceptance Criteria**:
- ✅ Sequential and random benchmarks working
- ✅ Direct I/O confirmed (results don't show cached speeds)
- ✅ IOPS and latency measured
- ✅ Test file properly cleaned up
- ✅ Results match expected ranges for drive types

---

### Phase 5: Memory Benchmark + Stress Testing (Week 9-10)

**Goals**:
- Memory bandwidth and latency measurement
- CPU, memory, and disk stress tests with thermal monitoring

**Tasks**:
1. Implement `MemoryBenchmarkWorker` (bandwidth, latency, allocation)
2. Implement `StressTestWorker` (CPU, memory, disk stress threads)
3. Implement `ThermalMonitor` (WMI temperature polling)
4. CPU stress: sustained all-core load
5. Memory stress: pattern write/verify
6. Disk stress: continuous I/O
7. Thermal monitoring with configurable abort threshold
8. Error detection and logging
9. Write unit tests

**Acceptance Criteria**:
- ✅ Memory bandwidth measured accurately
- ✅ Stress tests run for configurable duration
- ✅ Pattern errors detected in memory stress
- ✅ Thermal monitoring triggers abort at threshold
- ✅ No resource leaks during extended stress

---

### Phase 6: Report Generation (Week 11-12)

**Goals**:
- Generate professional reports in HTML, JSON, and CSV
- Pass/fail scoring with recommendations

**Tasks**:
1. Implement `DiagnosticReportGenerator`
2. HTML report with styled tables, color-coded status, charts
3. JSON report with full structured data
4. CSV export for spreadsheet analysis
5. Recommendations engine (e.g., "Replace drive", "Upgrade RAM")
6. Technician name and ticket number fields
7. Report auto-save to `_reports/` directory
8. Write tests for report content

**Acceptance Criteria**:
- ✅ HTML report is print-ready and professional
- ✅ JSON report is valid and complete
- ✅ CSV import works in Excel
- ✅ Recommendations are actionable and accurate

---

### Phase 7: UI Implementation (Week 13-15)

**Goals**:
- Complete Diagnostic & Benchmarking Panel UI
- Real-time updates, progress tracking, interactive controls

**Tasks**:
1. Implement `DiagnosticBenchmarkPanel` GUI
2. Hardware inventory display (collapsible sections)
3. SMART health table with color-coded status
4. Benchmark controls and results display
5. Stress test configuration and real-time monitoring
6. Thermal monitor with live temperature bars/chart
7. Report generation controls
8. "Run Full Diagnostic Suite" button (automated sequence)
9. Connect to `DiagnosticController` signals/slots
10. Settings persistence via `ConfigManager`
11. Add panel to `MainWindow` tab widget

**Acceptance Criteria**:
- ✅ UI complete and responsive
- ✅ Real-time updates during benchmarks and stress tests
- ✅ All controls functional
- ✅ Consistent with existing SAK UI style

---

### Phase 8: Testing & Polish (Week 16-18)

**Goals**:
- Test on diverse hardware configurations
- Performance optimization
- Edge case handling

**Tasks**:
1. Test on 10+ different hardware configs (desktop, laptop, NVMe, HDD, SATA SSD)
2. Test with missing components (no battery, no GPU, no SMART support)
3. Test benchmark reproducibility (< 3% variance)
4. Test stress test stability (1-hour continuous)
5. Test report generation with all combinations
6. Performance profiling (memory usage, CPU overhead)
7. Error handling for edge cases
8. Update README.md and THIRD_PARTY_LICENSES.md
9. Write user documentation

**Acceptance Criteria**:
- ✅ Works on diverse hardware
- ✅ Graceful handling of missing/unsupported hardware
- ✅ Benchmark scores reproducible
- ✅ Stress tests stable for extended durations
- ✅ Documentation complete

---

**Total Timeline**: 18 weeks (4.5 months)

---

## 📋 CMakeLists.txt Changes

### New Source Files
```cmake
# Add to CORE_SOURCES:
src/core/diagnostic_controller.cpp
src/core/hardware_inventory_scanner.cpp
src/core/smart_disk_analyzer.cpp
src/core/cpu_benchmark_worker.cpp
src/core/disk_benchmark_worker.cpp
src/core/memory_benchmark_worker.cpp
src/core/stress_test_worker.cpp
src/core/thermal_monitor.cpp
src/core/diagnostic_report_generator.cpp
include/sak/diagnostic_controller.h
include/sak/hardware_inventory_scanner.h
include/sak/smart_disk_analyzer.h
include/sak/cpu_benchmark_worker.h
include/sak/disk_benchmark_worker.h
include/sak/memory_benchmark_worker.h
include/sak/stress_test_worker.h
include/sak/thermal_monitor.h
include/sak/diagnostic_report_generator.h
include/sak/diagnostic_types.h

# Add to GUI_SOURCES:
src/gui/diagnostic_benchmark_panel.cpp
include/sak/diagnostic_benchmark_panel.h

# Add to PLATFORM_LIBS (Windows):
# dxgi.lib - for DXGI GPU enumeration
# pdh.lib  - for Performance Data Helper (CPU usage counters)
# wbemuuid.lib - for direct WMI COM access (optional, if moving away from PowerShell)
```

### Bundle smartmontools During Build
```cmake
# Copy smartmontools to output
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/smartmontools")
    add_custom_command(TARGET sak_utility POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/smartmontools
            $<TARGET_FILE_DIR:sak_utility>/tools/smartmontools
        COMMENT "Copying bundled smartmontools to output directory"
    )
endif()
```

---

## 📋 Configuration & Settings

### ConfigManager Extensions

```cpp
// Diagnostic & Benchmarking Settings
bool getDiagnosticAutoScanOnOpen() const;       // Auto-scan hardware when panel opens
void setDiagnosticAutoScanOnOpen(bool enabled);

QString getDiagnosticReportOutputDir() const;   // Report save directory
void setDiagnosticReportOutputDir(const QString& path);

QString getDiagnosticTechnicianName() const;    // Saved technician name
void setDiagnosticTechnicianName(const QString& name);

int getDiagnosticStressTestDuration() const;    // Default stress duration (minutes)
void setDiagnosticStressTestDuration(int minutes);

double getDiagnosticThermalLimit() const;       // Stress test thermal limit
void setDiagnosticThermalLimit(double celsius);

int getDiagnosticDiskBenchSizeMB() const;       // Disk benchmark test file size
void setDiagnosticDiskBenchSizeMB(int sizeMB);
```

**Default Values**:
```cpp
diagnostic/auto_scan_on_open = true
diagnostic/report_output_dir = "%USERPROFILE%\\Documents\\SAK_Reports"
diagnostic/technician_name = ""
diagnostic/stress_test_duration = 10          // minutes
diagnostic/thermal_limit = 95.0              // °C
diagnostic/disk_bench_size_mb = 1024         // 1 GB test file
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_hardware_inventory_scanner.cpp**:
- WMI query parsing (mock JSON output)
- CPU info extraction
- Memory module enumeration
- Storage device detection
- GPU enumeration
- Battery health calculation
- Missing component handling

**test_smart_disk_analyzer.cpp**:
- smartctl JSON parsing (SATA)
- smartctl JSON parsing (NVMe)
- Health assessment (Healthy/Warning/Critical thresholds)
- Warning generation
- Recommendation generation
- Edge cases (USB drives, no SMART support)

**test_cpu_benchmark_worker.cpp**:
- Prime sieve correctness (verify prime count)
- Matrix multiply correctness (verify result sample)
- Score calculation
- Thread scaling calculation
- Cancellation handling

**test_disk_benchmark_worker.cpp**:
- Test file creation and cleanup
- Sequential I/O measurement (verify non-zero)
- Random I/O measurement (verify non-zero)
- IOPS calculation
- Latency calculation

**test_memory_benchmark_worker.cpp**:
- Bandwidth measurement (verify non-zero)
- Latency measurement (verify within expected range)
- Allocation stress

**test_stress_test_worker.cpp**:
- CPU stress thread execution
- Memory pattern verification
- Thermal abort trigger
- Duration compliance
- Error detection

**test_diagnostic_report_generator.cpp**:
- HTML report generation (valid HTML)
- JSON report generation (valid JSON, correct structure)
- CSV report generation (valid CSV)
- Recommendation engine logic

### Integration Tests

**test_diagnostic_integration.cpp**:
- Full diagnostic suite execution (hardware scan → SMART → benchmarks → report)
- smartctl integration (live drive scan)
- Report generation with real data
- Controller state machine transitions

### Manual Testing

1. **Desktop PC** (NVMe SSD, HDD, discrete GPU, no battery)
2. **Laptop** (SATA SSD, integrated GPU, battery)
3. **Server** (multiple drives, ECC RAM, no GPU)
4. **Thin client** (eMMC storage, limited RAM)
5. **VM** (virtualized hardware, no SMART)

---

## 🚧 Limitations & Challenges

### Technical Limitations

**WMI Temperature Accuracy**:
- ⚠️ `MSAcpi_ThermalZoneTemperature` reports ACPI zone temperature, not per-core
- ⚠️ Some systems don't expose temperature via WMI at all
- **Mitigation**: Fall back to "N/A" with explanation; recommend external monitoring for detailed thermal analysis

**Disk Benchmark Cache Bypass**:
- ⚠️ `FILE_FLAG_NO_BUFFERING` requires sector-aligned buffers (512 or 4096 bytes)
- ⚠️ Some drives have internal cache that can't be bypassed
- **Mitigation**: Use sufficiently large test files (1 GB+) to exceed internal cache

**GPU Benchmarking**:
- ❌ Full GPU benchmarking (3D rendering) not in scope — would require DirectX/Vulkan compute shaders
- **Mitigation**: Report GPU info and driver status; recommend dedicated GPU benchmarks (FurMark, 3DMark)

**SMART via USB**:
- ⚠️ USB-to-SATA bridges often don't pass SMART commands through
- **Mitigation**: smartctl has `-d` flag for USB bridges; warn user if SMART not available

**Admin Privileges**:
- ❌ SMART reading and thermal monitoring require administrator privileges
- ✅ SAK Utility already runs as admin (manifest)

### Workarounds

**No Temperature Available**:
```cpp
// If WMI returns no temperature data, display graceful fallback
if (temperature < 0) {
    // Show "N/A — Temperature monitoring not available on this system"
    // Stress test uses CPU usage as proxy for load (target 100%)
}
```

**VMs and Thin Clients**:
```cpp
// Detect virtualized environment
bool isVirtualMachine = wmiQuery("Win32_ComputerSystem", "Model")
    .toString().contains(QRegularExpression("Virtual|VMware|VirtualBox|Hyper-V|QEMU"));
// Adjust expectations and display warning
```

---

## 🎯 Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| Hardware scan time | < 5 seconds | High |
| SMART scan time (per drive) | < 10 seconds | High |
| CPU benchmark time | < 60 seconds | Medium |
| Disk benchmark time | < 3 minutes | Medium |
| Memory benchmark time | < 30 seconds | Medium |
| Benchmark score variance | < 3% between runs | Critical |
| Stress test stability | No crashes in 24-hour test | Critical |
| Report generation | < 5 seconds | Medium |
| Supported drive types | SATA, NVMe, USB, HDD, SSD | High |
| Missing hardware handling | Graceful N/A, no crashes | Critical |

---

## 🔒 Security Considerations

### SMART Data Privacy
- SMART data contains serial numbers — ensure reports are handled securely
- Allow technician to redact serial numbers from customer-facing reports

### Stress Test Safety
- Thermal auto-abort is critical — must work reliably to prevent hardware damage
- Memory stress should not exceed configured percentage to prevent system instability
- Disk stress should warn about SSD wear (write amplification)

### Benchmark Integrity
- Use fixed random seeds for reproducibility
- Include hardware/OS info in results to prevent invalid comparisons
- Reports include timestamps to detect configuration changes between runs

---

## 💡 Future Enhancements (Post-v1.0)

### v1.1 - Advanced Features
- **GPU Compute Benchmark**: DirectX 12 compute shader benchmark
- **Network-attached Storage Testing**: Test NAS/SAN performance via SMB/NFS
- **Advanced Thermal Monitoring**: Support for LibreHardwareMonitor integration for per-core temps, fan RPM, voltages
- **Historical Tracking**: Store benchmark results locally, chart performance over time
- **Custom Benchmark Profiles**: Save/load benchmark configurations for specific use cases

### v1.2 - Enterprise Features
- **Remote Diagnostics**: Run diagnostic suite remotely via SAK orchestration network
- **Fleet Dashboard Integration**: Push diagnostic JSON to central dashboard
- **Automated Scheduling**: Schedule quarterly diagnostic runs
- **Custom Thresholds**: Per-customer or per-hardware-model SMART thresholds
- **PDF Report Generation**: High-quality PDF reports with charts (via Qt PDF or wkhtmltopdf)

---

## 📚 Resources

### Official Documentation
- [smartmontools Documentation](https://www.smartmontools.org/wiki/TocDoc)
- [WMI Classes Reference](https://learn.microsoft.com/windows/win32/cimwin32prov/computer-system-hardware-classes)
- [DXGI Overview](https://learn.microsoft.com/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)
- [Windows Performance Counters](https://learn.microsoft.com/windows/win32/perfctrs/performance-counters-portal)
- [FILE_FLAG_NO_BUFFERING](https://learn.microsoft.com/windows/win32/fileio/file-buffering)
- [SMART Attribute Reference](https://en.wikipedia.org/wiki/S.M.A.R.T.#Known_ATA_S.M.A.R.T._attributes)

### Community Resources
- [CrystalDiskMark Methodology](https://crystalmark.info/en/software/crystaldiskmark/)
- [Geekbench Scoring](https://www.geekbench.com/doc/geekbench6-benchmark-internals.pdf)
- [STREAM Memory Benchmark](https://www.cs.virginia.edu/stream/)

---

## 📞 Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: February 25, 2026  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation
