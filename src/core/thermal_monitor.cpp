// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.cpp
/// @brief Real-time thermal sensor monitoring implementation

#include "sak/thermal_monitor.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDateTime>
#include <QProcess>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

ThermalMonitor::ThermalMonitor(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ThermalMonitor::onTimerTick);
}

ThermalMonitor::~ThermalMonitor() {
    stop();
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

    // Perform an initial poll immediately, then schedule next via single-shot
    onTimerTick();
}

void ThermalMonitor::stop() {
    if (m_timer.isActive()) {
        m_timer.stop();
        logInfo("Thermal monitor stopped");
    }
}

bool ThermalMonitor::isRunning() const {
    return m_timer.isActive();
}

QVector<ThermalReading> ThermalMonitor::pollOnce() {
    QVector<ThermalReading> readings;
    const QDateTime now = QDateTime::currentDateTime();

    // CPU temperature
    const double cpu_temp = queryCpuTemperature();
    if (cpu_temp > 0) {
        readings.append({"CPU Package", cpu_temp, now});
    }

    // GPU temperature
    const double gpu_temp = queryGpuTemperature();
    if (gpu_temp > 0) {
        readings.append({"GPU", gpu_temp, now});
    }

    return readings;
}

void ThermalMonitor::clearHistory() {
    m_history.clear();
}

// ============================================================================
// Timer Callback
// ============================================================================

void ThermalMonitor::onTimerTick() {
    Q_ASSERT(!m_history.empty());
    Q_ASSERT(!m_history.isEmpty());
    const auto readings = pollOnce();

    // Store in history
    m_history.append(readings);

    // Trim history to ~30 minutes (at 2-second polling = ~900 readings,
    // each poll returns up to 2 sensors, so ~1800 entries)
    constexpr int kMaxHistoryEntries = 1800;
    if (m_history.size() > kMaxHistoryEntries) {
        m_history.remove(0, m_history.size() - kMaxHistoryEntries);
    }

    // Check warning thresholds
    for (const auto& reading : readings) {
        if (reading.component == "CPU Package" &&
            reading.temperature_celsius >= m_cpu_warning_threshold) {
            Q_EMIT temperatureWarning(reading.component, reading.temperature_celsius);
        } else if (reading.component == "GPU" &&
                   reading.temperature_celsius >= m_gpu_warning_threshold) {
            Q_EMIT temperatureWarning(reading.component, reading.temperature_celsius);
        } else if (reading.component.startsWith("Disk") &&
                   reading.temperature_celsius >= m_disk_warning_threshold) {
            Q_EMIT temperatureWarning(reading.component, reading.temperature_celsius);
        }
    }

    Q_EMIT readingsUpdated(readings);

    // Restart single-shot timer after poll completes
    // (prevents accumulation of concurrent processes)
    m_timer.start(m_interval_ms);
}

// ============================================================================
// Temperature Queries
// ============================================================================

double ThermalMonitor::queryCpuTemperature() {
    QProcess ps;
    ps.setProcessChannelMode(QProcess::MergedChannels);
    ps.start("powershell.exe",
             {"-NoProfile",
              "-NoLogo",
              "-Command",
              "Get-CimInstance -Namespace root/WMI -ClassName MSAcpi_ThermalZoneTemperature "
              "| Select-Object -First 1 -ExpandProperty CurrentTemperature"});

    if (!ps.waitForStarted(sak::kTimeoutProcessStartMs)) {
        return -1.0;
    }
    if (!ps.waitForFinished(sak::kTimeoutProcessShortMs)) {
        ps.kill();
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

double ThermalMonitor::queryGpuTemperature() {
    // Try NVIDIA first via nvidia-smi (commonly available on NVIDIA systems)
    QProcess nv;
    nv.setProcessChannelMode(QProcess::MergedChannels);
    nv.start("nvidia-smi", {"--query-gpu=temperature.gpu", "--format=csv,noheader,nounits"});

    if (!nv.waitForStarted(sak::kTimeoutProcessStartMs)) {
        // nvidia-smi not available — fall through to fallback
    } else if (nv.waitForFinished(sak::kTimeoutThermalQueryMs) && nv.exitCode() == 0) {
        const QString output = QString::fromUtf8(nv.readAllStandardOutput()).trimmed();
        bool ok = false;
        const double temp = output.split('\n').first().trimmed().toDouble(&ok);
        if (ok && temp > 0) {
            return temp;
        }
    }

    // Fallback: WMI Win32_VideoController doesn't have temperature
    // For AMD, we'd need radeon-profile or similar — not universally available
    return -1.0;
}

}  // namespace sak
