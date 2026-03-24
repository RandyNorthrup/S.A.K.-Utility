// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.cpp
/// @brief Real-time thermal sensor monitoring implementation

#include "sak/thermal_monitor.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDateTime>
#include <QProcess>
#include <QtConcurrent>

namespace sak {

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
        m_poll_watcher.waitForFinished();
    }
}

// ============================================================================
// Public API
// ============================================================================

void ThermalMonitor::start(int interval_ms) {
    if (m_timer.isActive()) {
        m_timer.stop();
    }

    m_interval_ms = interval_ms;
    logInfo("Thermal monitor started ({}ms interval)", interval_ms);

    // Fire initial poll immediately
    onTimerTick();
}

void ThermalMonitor::stop() {
    if (m_timer.isActive()) {
        m_timer.stop();
        logInfo("Thermal monitor stopped");
    }
}

bool ThermalMonitor::isRunning() const {
    return m_timer.isActive() || m_poll_watcher.isRunning();
}

QVector<ThermalReading> ThermalMonitor::pollOnce() {
    QProcess ps;
    ps.setProcessChannelMode(QProcess::MergedChannels);
    ps.start("powershell.exe", {"-NoProfile", "-NoLogo", "-Command", buildCombinedThermalScript()});

    if (!ps.waitForStarted(sak::kTimeoutProcessStartMs)) {
        return {};
    }
    if (!ps.waitForFinished(sak::kTimeoutThermalQueryMs)) {
        ps.kill();
        ps.waitForFinished(sak::kTimeoutProcessStartMs);
        return {};
    }

    return parseThermalOutput(QString::fromUtf8(ps.readAllStandardOutput()));
}

void ThermalMonitor::clearHistory() {
    m_history.clear();
}

// ============================================================================
// Timer Callback (main thread)
// ============================================================================

void ThermalMonitor::onTimerTick() {
    if (m_poll_watcher.isRunning()) {
        m_timer.start(m_interval_ms);
        return;
    }

    // Run pollOnce() on the thread pool — no UI blocking
    m_poll_watcher.setFuture(QtConcurrent::run(&ThermalMonitor::pollOnce));
}

void ThermalMonitor::onPollComplete() {
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

        // GPU: try nvidia-smi at well-known paths, then PATH
        "$nvp=@("
        "'C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe',"
        "'C:\\Windows\\System32\\nvidia-smi.exe',"
        "'nvidia-smi');"
        "foreach($p in $nvp){"
        "try{"
        "$o=&$p --query-gpu=temperature.gpu "
        "--format=csv,noheader,nounits 2>$null;"
        "if($LASTEXITCODE -eq 0 -and $o){"
        "$r['gpu']=[double]$o.Trim().Split(\"`n\")[0].Trim();"
        "break"
        "}}catch{}};"

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
        const int eq_pos = trimmed.indexOf('=');
        if (eq_pos <= 0) {
            continue;
        }

        const auto key = trimmed.left(eq_pos).toLower();
        bool ok = false;
        const double temp = trimmed.mid(eq_pos + 1).toDouble(&ok);
        if (!ok || temp <= 0) {
            continue;
        }

        if (key == QLatin1String("cpu")) {
            readings.append({"CPU Package", temp, now});
        } else if (key == QLatin1String("gpu")) {
            readings.append({"GPU", temp, now});
        } else if (key.startsWith(QLatin1String("disk"))) {
            readings.append({QString("Disk %1").arg(key.mid(4)), temp, now});
        }
    }
    return readings;
}

// ============================================================================
// Process Readings (history, thresholds, signals)
// ============================================================================

void ThermalMonitor::processReadings(const QVector<ThermalReading>& readings) {
    m_history.append(readings);
    if (m_history.size() > kMaxHistoryEntries) {
        m_history.remove(0, m_history.size() - kMaxHistoryEntries);
    }

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
    QProcess ps;
    ps.setProcessChannelMode(QProcess::MergedChannels);
    ps.start("powershell.exe",
             {"-NoProfile",
              "-NoLogo",
              "-Command",
              "Get-CimInstance -Namespace root/WMI "
              "-ClassName MSAcpi_ThermalZoneTemperature "
              "| Select-Object -First 1 "
              "-ExpandProperty CurrentTemperature"});

    if (!ps.waitForStarted(sak::kTimeoutProcessStartMs)) {
        return -1.0;
    }
    if (!ps.waitForFinished(sak::kTimeoutProcessShortMs)) {
        ps.kill();
        ps.waitForFinished(sak::kTimeoutProcessStartMs);
        return -1.0;
    }

    if (ps.exitCode() != 0) {
        return -1.0;
    }

    const QString output = QString::fromUtf8(ps.readAllStandardOutput()).trimmed();
    bool ok = false;
    const double raw_value = output.toDouble(&ok);
    if (!ok || raw_value <= 0) {
        return -1.0;
    }

    // WMI returns temperature in tenths of Kelvin
    return (raw_value / 10.0) - 273.15;
}

}  // namespace sak
