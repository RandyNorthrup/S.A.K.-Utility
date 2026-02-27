// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.h
/// @brief Real-time thermal sensor monitoring

#pragma once

#include "sak/diagnostic_types.h"

#include <QObject>
#include <QTimer>
#include <QVector>

#include <atomic>

namespace sak {

/// @brief Polls system thermal sensors at a configurable interval
///
/// Queries CPU temperature via WMI MSAcpi_ThermalZoneTemperature and
/// disk temperatures from SMART data. Emits readings at each poll
/// interval for UI display and thermal history tracking.
///
/// Note: MSAcpi_ThermalZoneTemperature requires administrator privileges
/// and is not available on all systems. Unavailable sensors report -1.0°C.
///
/// Usage:
/// @code
///   ThermalMonitor monitor;
///   connect(&monitor, &ThermalMonitor::readingsUpdated, this, &Panel::onTemps);
///   monitor.start(2000); // poll every 2 seconds
///   // ...
///   monitor.stop();
/// @endcode
class ThermalMonitor : public QObject {
    Q_OBJECT

public:
    /// @brief Construct a ThermalMonitor
    /// @param parent Parent QObject
    explicit ThermalMonitor(QObject* parent = nullptr);
    ~ThermalMonitor() override;

    // Non-copyable, non-movable
    ThermalMonitor(const ThermalMonitor&) = delete;
    ThermalMonitor& operator=(const ThermalMonitor&) = delete;
    ThermalMonitor(ThermalMonitor&&) = delete;
    ThermalMonitor& operator=(ThermalMonitor&&) = delete;

    /// @brief Start periodic temperature polling
    /// @param interval_ms Poll interval in milliseconds (default: 2000)
    void start(int interval_ms = 2000);

    /// @brief Stop temperature polling
    void stop();

    /// @brief Check if monitoring is active
    /// @return true if the timer is running
    [[nodiscard]] bool isRunning() const;

    /// @brief Perform a single poll and return current readings
    /// @return Vector of thermal readings for all available sensors
    [[nodiscard]] QVector<ThermalReading> pollOnce();

    /// @brief Get the history of all readings since monitoring started
    /// @return All accumulated thermal readings
    [[nodiscard]] const QVector<ThermalReading>& history() const { return m_history; }

    /// @brief Clear the reading history
    void clearHistory();

    /// @brief Query CPU thermal zone temperature from WMI
    /// @return Temperature in Celsius, or -1.0 if unavailable
    /// @note Shared utility — also used by StressTestWorker
    [[nodiscard]] static double queryCpuTemperature();

Q_SIGNALS:
    /// @brief Emitted at each poll interval with current readings
    /// @param readings Current temperature for all detected sensors
    void readingsUpdated(const QVector<sak::ThermalReading>& readings);

    /// @brief Emitted when any sensor exceeds a warning threshold
    /// @param component The sensor that triggered the warning
    /// @param temperature Current temperature in Celsius
    void temperatureWarning(const QString& component, double temperature);

private Q_SLOTS:
    /// @brief Timer callback — performs a poll and emits signals
    void onTimerTick();

private:
    /// @brief Query GPU temperature from WMI
    /// @return Temperature in Celsius, or -1.0 if unavailable
    [[nodiscard]] double queryGpuTemperature();

    QTimer m_timer;
    int m_interval_ms{2000};                  ///< Configured poll interval
    QVector<ThermalReading> m_history;

    /// Warning thresholds (Celsius)
    double m_cpu_warning_threshold{85.0};
    double m_gpu_warning_threshold{90.0};
    double m_disk_warning_threshold{55.0};
};

} // namespace sak
