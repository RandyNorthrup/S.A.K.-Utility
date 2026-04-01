// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.h
/// @brief Real-time thermal sensor monitoring

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/layout_constants.h"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QVector>

namespace sak {

/// @brief Polls system thermal sensors at a configurable interval
///
/// Uses a single PowerShell process per poll cycle to query CPU (WMI ACPI
/// thermal zone), GPU (nvidia-smi / AMD WMI), and disk (Storage Reliability
/// Counter) temperatures. Polling runs on a thread-pool thread via
/// QtConcurrent so the UI thread is never blocked.
///
/// Note: MSAcpi_ThermalZoneTemperature and StorageReliabilityCounter require
/// administrator privileges. Unavailable sensors are omitted from readings.
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

    /// @brief Perform a single synchronous poll and return current readings
    /// @return Vector of thermal readings for all available sensors
    /// @note Thread-safe -- creates only local objects
    [[nodiscard]] static QVector<ThermalReading> pollOnce();

    /// @brief Get the history of all readings since monitoring started
    /// @return All accumulated thermal readings
    [[nodiscard]] const QVector<ThermalReading>& history() const { return m_history; }

    /// @brief Clear the reading history
    void clearHistory();

    /// @brief Query CPU thermal zone temperature from WMI
    /// @return Temperature in Celsius, or -1.0 if unavailable
    /// @note Shared utility -- also used by StressTestWorker
    [[nodiscard]] static double queryCpuTemperature();

    /// @brief Check if the last poll included CPU temperature data
    /// @return true if the most recent readings contain a CPU sensor
    [[nodiscard]] bool hasCpuTemperature() const;

Q_SIGNALS:
    /// @brief Emitted at each poll interval with current readings
    /// @param readings Current temperature for all detected sensors
    void readingsUpdated(const QVector<sak::ThermalReading>& readings);

    /// @brief Emitted when any sensor exceeds a warning threshold
    /// @param component The sensor that triggered the warning
    /// @param temperature Current temperature in Celsius
    void temperatureWarning(const QString& component, double temperature);

private Q_SLOTS:
    /// @brief Timer callback -- launches async poll
    void onTimerTick();

    /// @brief Async poll finished -- processes results on main thread
    void onPollComplete();

private:
    /// @brief Build the combined PowerShell script that queries all sensors
    /// @return PowerShell script string
    [[nodiscard]] static QString buildCombinedThermalScript();

    /// @brief Parse key=value output from the combined thermal script
    /// @param output Raw stdout from PowerShell
    /// @return Parsed thermal readings
    [[nodiscard]] static QVector<ThermalReading> parseThermalOutput(const QString& output);

    /// @brief Process completed readings: history, thresholds, signals
    void processReadings(const QVector<ThermalReading>& readings);

    QTimer m_timer;
    QFutureWatcher<QVector<ThermalReading>> m_poll_watcher;
    int m_interval_ms{sak::kTimerBroadcastMs};  ///< Configured poll interval
    QVector<ThermalReading> m_history;

    /// Warning thresholds (Celsius)
    static constexpr double kCpuWarningThreshold = 85.0;
    static constexpr double kGpuWarningThreshold = 90.0;
    static constexpr double kDiskWarningThreshold = 55.0;
    static constexpr int kMaxHistoryEntries = 1800;
};

}  // namespace sak
