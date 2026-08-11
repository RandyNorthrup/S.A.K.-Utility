// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.cpp
/// @brief Real-time thermal sensor monitoring implementation

#include "sak/thermal_monitor.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace sak {

namespace {
constexpr qsizetype kDiskKeyPrefixLength = 4;
constexpr double kWmiTenthsKelvinDivisor = 10.0;
constexpr double kKelvinZeroCelsius = 273.15;
// No silicon or drive sensor reports above this; a larger value is telemetry corruption,
// not a temperature, and must not reach the thresholds or the UI.
constexpr double kMaxPlausibleCelsius = 150.0;

// A sensor value must be a FINITE Celsius reading inside the plausible range.
// QString::toDouble accepts "nan"/"inf", and a NaN passes a bare `temp <= 0` test, enters
// the reading set, then silently fails every threshold comparison in processReadings
// (NaN >= x is false) -- a warning that can never fire. Reject instead of coercing.
[[nodiscard]] bool isPlausibleCelsius(double temp) {
    return std::isfinite(temp) && temp > 0.0 && temp <= kMaxPlausibleCelsius;
}

// True only for the "disk<DeviceId>" keys the combined script emits: the suffix must be a
// non-empty run of digits. Any other "disk*" key is not a device identity we can trust, so
// the line is dropped instead of becoming a component named after arbitrary text.
[[nodiscard]] bool isDiskDeviceId(const QString& device_id) {
    if (device_id.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(device_id, [](const QChar ch) { return ch.isDigit(); });
}
}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

ThermalMonitor::ThermalMonitor(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ThermalMonitor::onTimerTick);
    connect(&m_poll_watcher,
            &QFutureWatcher<QVector<ThermalReading>>::finished,
            this,
            &ThermalMonitor::onPollComplete);
}

ThermalMonitor::~ThermalMonitor() {
    stop();
    if (m_poll_watcher.isRunning()) {
        // SAK-ALLOW-BLOCKING: the pool task writes its readings into m_poll_watcher, which
        // dies with this object. A single sensor poll is short, bounded work; abandoning it
        // would hand the pool a dangling watcher.
        m_poll_watcher.waitForFinished();
    }
}

// ============================================================================
// Public API
// ============================================================================

int ThermalMonitor::clampPollIntervalMs(int requested) {
    return requested >= kMinThermalPollIntervalMs ? requested : kMinThermalPollIntervalMs;
}

void ThermalMonitor::start(int interval_ms) {
    if (m_timer.isActive()) {
        m_timer.stop();
    }

    // Clamp: a zero interval busy-spins the sensor query; a negative one is
    // invalid. Never arm the timer with a non-positive interval.
    m_interval_ms = clampPollIntervalMs(interval_ms);
    m_active = true;
    logInfo("Thermal monitor started ({}ms interval)", m_interval_ms);

    // Fire initial poll immediately
    onTimerTick();
}

void ThermalMonitor::stop() {
    // Record stop intent FIRST: during an in-flight poll the timer is inactive, so without this a
    // stop() would be a no-op and onPollComplete would re-arm the timer, resuming forever.
    m_active = false;
    if (m_timer.isActive()) {
        m_timer.stop();
        logInfo("Thermal monitor stopped");
    }
}

bool ThermalMonitor::isRunning() const {
    return m_timer.isActive() || m_poll_watcher.isRunning();
}

QVector<ThermalReading> ThermalMonitor::pollOnce() {
    bool ignored = false;
    return pollOnce(ignored);
}

QVector<ThermalReading> ThermalMonitor::pollOnce(bool& queryOk) {
    queryOk = false;
    // System32-qualified interpreter, never a bare "powershell.exe": the poll can run
    // from an elevated session, so a PATH/CWD-planted powershell must not satisfy it.
    // Unresolvable -> FAILED query (queryOk stays false).
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        return {};
    }
    const auto result = sak::runProcess(powershell,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NoLogo"),
                                         QStringLiteral("-Command"),
                                         buildCombinedThermalScript()},
                                        sak::kTimeoutThermalQueryMs);
    if (!result.succeeded()) {
        return {};  // query FAILED (non-zero exit / timeout) -> queryOk stays false
    }

    // The script runs to completion and emits key=value lines only for available sensors; an
    // empty parse is a genuine "no readable sensors", not a failure.
    queryOk = true;
    return parseThermalOutput(result.std_out);
}

// ============================================================================
// Timer Callback (main thread)
// ============================================================================

void ThermalMonitor::onTimerTick() {
    if (m_poll_watcher.isRunning()) {
        m_timer.start(m_interval_ms);
        return;
    }

    // Run pollOnce() on the thread pool -- no UI blocking.
    // Lambda (not &pollOnce) to disambiguate the overload set: pollOnce() vs pollOnce(bool&).
    // The bool& overload is taken deliberately: a FAILED query (unresolvable interpreter,
    // non-zero exit, timeout) also yields an empty vector, so without this the periodic path
    // would report a sensor failure as a normal "no readable sensors" tick and say nothing.
    m_poll_watcher.setFuture(QtConcurrent::run([] {
        bool query_ok = false;
        QVector<ThermalReading> readings = ThermalMonitor::pollOnce(query_ok);
        if (!query_ok) {
            logWarning("Thermal sensor query FAILED; this poll reports no readings");
        }
        return readings;
    }));
}

void ThermalMonitor::onPollComplete() {
    // If stop() was called while this poll was in flight, drop the stale result and do NOT re-arm
    // the timer (this also stops the dtor's stop()+waitForFinished() teardown from resurrecting
    // it).
    if (!m_active) {
        return;
    }
    processReadings(m_poll_watcher.result());
    m_timer.start(m_interval_ms);
}

// ============================================================================
// Combined Thermal Script
// ============================================================================

QString ThermalMonitor::buildCombinedThermalScript() {
    // Single PowerShell invocation that queries CPU, GPU, and all disks.
    // Output format: key=value lines (e.g. "cpu=52.3", "gpu=61", "disk0=38")
    return QStringLiteral(
        "$r=@{};"

        // CPU via WMI ACPI thermal zone (admin required)
        "try{"
        "$t=Get-CimInstance -Namespace root/WMI "
        "-ClassName MSAcpi_ThermalZoneTemperature "
        "-ErrorAction Stop|Select-Object -First 1;"
        "if($t.CurrentTemperature -gt 0){"
        "$r['cpu']=[math]::Round(($t.CurrentTemperature/10)-273.15,1)"
        "}}catch{};"

        // GPU: nvidia-smi at its absolute install paths ONLY. A bare "nvidia-smi" resolved
        // through PATH/CWD is a forbidden fallback here -- the poll can run from an elevated
        // session, where an attacker-planted nvidia-smi would then execute as admin.
        // nvidia-smi prints one line per GPU: report the HOTTEST so a second, overheating
        // card is never hidden behind a cool first one.
        "$nvp=@("
        "'C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe',"
        "'C:\\Windows\\System32\\nvidia-smi.exe');"
        "foreach($p in $nvp){"
        "if(-not (Test-Path -LiteralPath $p -PathType Leaf)){continue};"
        "try{"
        "$o=&$p --query-gpu=temperature.gpu "
        "--format=csv,noheader,nounits 2>$null;"
        "if($LASTEXITCODE -eq 0 -and $o){"
        "$g=@($o)|ForEach-Object{$_.ToString().Split(\"`n\")}|"
        "ForEach-Object{$_.Trim()}|Where-Object{$_}|ForEach-Object{[double]$_};"
        "if($g){"
        "$r['gpu']=($g|Measure-Object -Maximum).Maximum;"
        "break"
        "}}}catch{}};"

        // Disk temps via StorageReliabilityCounter (admin required)
        "try{"
        "Get-PhysicalDisk -ErrorAction Stop|ForEach-Object{"
        "$c=$_|Get-StorageReliabilityCounter "
        "-ErrorAction SilentlyContinue;"
        "if($c -and $c.Temperature -gt 0){"
        "$r[\"disk$($_.DeviceId)\"]=$c.Temperature"
        "}}}catch{};"

        // Output key=value pairs
        "$r.GetEnumerator()|ForEach-Object{"
        "\"$($_.Key)=$($_.Value)\""
        "}");
}

QVector<ThermalReading> ThermalMonitor::parseThermalOutput(const QString& output) {
    QVector<ThermalReading> readings;
    const QDateTime now = QDateTime::currentDateTime();

    const auto lines = output.split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        const auto trimmed = line.trimmed();
        const int eq_pos = static_cast<int>(trimmed.indexOf('='));
        if (eq_pos <= 0) {
            continue;
        }

        const auto key = trimmed.left(eq_pos).toLower();
        bool ok = false;
        const double temp = trimmed.mid(eq_pos + 1).toDouble(&ok);
        if (!ok || !isPlausibleCelsius(temp)) {
            continue;
        }

        if (key == QLatin1String("cpu")) {
            readings.append(
                {.component = "CPU Package", .temperature_celsius = temp, .timestamp = now});
        } else if (key == QLatin1String("gpu")) {
            readings.append({.component = "GPU", .temperature_celsius = temp, .timestamp = now});
        } else if (key.startsWith(QLatin1String("disk"))) {
            const QString device_id = key.mid(kDiskKeyPrefixLength);
            if (!isDiskDeviceId(device_id)) {
                continue;
            }
            readings.append({.component = QString("Disk %1").arg(device_id),
                             .temperature_celsius = temp,
                             .timestamp = now});
        }
    }
    return readings;
}

// ============================================================================
// Process Readings (thresholds, signals)
// ============================================================================

void ThermalMonitor::processReadings(const QVector<ThermalReading>& readings) {
    for (const auto& reading : readings) {
        const double temp = reading.temperature_celsius;
        if (reading.component == "CPU Package" && temp >= kCpuWarningThreshold) {
            Q_EMIT temperatureWarning(reading.component, temp);
        } else if (reading.component == "GPU" && temp >= kGpuWarningThreshold) {
            Q_EMIT temperatureWarning(reading.component, temp);
        } else if (reading.component.startsWith("Disk") && temp >= kDiskWarningThreshold) {
            Q_EMIT temperatureWarning(reading.component, temp);
        }
    }

    Q_EMIT readingsUpdated(readings);
}

// ============================================================================
// Standalone CPU Query (used by StressTestWorker)
// ============================================================================

double ThermalMonitor::queryCpuTemperature() {
    // System32-qualified interpreter only; unresolvable -> no reading (-1.0), never a
    // PATH/CWD-resolved launch.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        return -1.0;
    }
    const auto result = sak::runProcess(powershell,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NoLogo"),
                                         QStringLiteral("-Command"),
                                         QStringLiteral("Get-CimInstance -Namespace root/WMI "
                                                        "-ClassName MSAcpi_ThermalZoneTemperature "
                                                        "| Select-Object -First 1 "
                                                        "-ExpandProperty CurrentTemperature")},
                                        sak::kTimeoutProcessShortMs);

    if (!result.succeeded()) {
        return -1.0;
    }

    const QString output = result.std_out.trimmed();
    bool ok = false;
    const double raw_value = output.toDouble(&ok);
    if (!ok || !std::isfinite(raw_value) || raw_value <= 0) {
        return -1.0;
    }

    // WMI returns temperature in tenths of Kelvin
    const double celsius = (raw_value / kWmiTenthsKelvinDivisor) - kKelvinZeroCelsius;
    // Fail closed: a non-finite or out-of-range value would sail through the caller's
    // "temp >= abort threshold" test (NaN compares false) and defeat thermal protection.
    return isPlausibleCelsius(celsius) ? celsius : -1.0;
}

}  // namespace sak
