// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.h
/// @brief Real-time thermal sensor monitoring

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/layout_constants.h"

#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

class ThermalMonitorTests;

namespace sak {

inline constexpr int kDefaultThermalPollIntervalMs = kThermalReadoutPollIntervalMs;

/// @brief One periodic poll's outcome, carried from the pool thread back to the monitor.
struct ThermalPollResult {
    QVector<ThermalReading> readings;
    /// "status_<sensor>=unavailable:<reason>" lines for sensors the script could not read.
    QStringList sensor_statuses;
    bool query_ok{false};
};

/// @brief Floor for the poll interval. A zero interval makes QTimer fire
///        continuously (busy-spinning the sensor query); a negative interval is
///        invalid. Callers passing <= 0 are clamped up to this.
inline constexpr int kMinThermalPollIntervalMs = 250;

/// @brief Polls system thermal sensors at a configurable interval
///
/// Uses a single PowerShell process per poll cycle to query CPU (WMI ACPI
/// thermal zone), GPU (nvidia-smi / AMD WMI), and disk (Storage Reliability
/// Counter) temperatures. Polling runs on a thread-pool thread via
/// QtConcurrent so the UI thread is never blocked.
///
/// Note: MSAcpi_ThermalZoneTemperature and StorageReliabilityCounter require
/// administrator privileges. Legitimately unavailable sensors are omitted from readings and
/// logged with an explicit unavailable reason. Query failures fail the poll.
///
/// Usage:
/// @code
///   ThermalMonitor monitor;
///   connect(&monitor, &ThermalMonitor::readingsUpdated, this, &Panel::onTemps);
///   monitor.start(); // kDefaultThermalPollIntervalMs
///   // ...
///   monitor.stop();
/// @endcode
class ThermalMonitor : public QObject {
    friend class ::ThermalMonitorTests;

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
    /// @param interval_ms Poll interval in milliseconds (default: 5000). Values
    ///        <= 0 are clamped up to kMinThermalPollIntervalMs.
    void start(int interval_ms = kDefaultThermalPollIntervalMs);

    /// @brief Clamp a requested poll interval to a safe, non-busy floor.
    /// @return @p requested when it is >= kMinThermalPollIntervalMs, else the
    ///         floor. Pure + static for unit testing.
    [[nodiscard]] static int clampPollIntervalMs(int requested);

    /// @brief Stop temperature polling
    void stop();

    /// @brief Check if monitoring is active
    /// @return true if the timer is running
    [[nodiscard]] bool isRunning() const;

    /// @brief Perform a single synchronous poll and return current readings
    /// @return Vector of thermal readings for all available sensors
    /// @note Thread-safe -- creates only local objects
    [[nodiscard]] static QVector<ThermalReading> pollOnce();

    /// @brief Single synchronous poll, reporting whether the query SUCCEEDED.
    /// @param query_ok set true when the sensor-query process ran to completion (an empty result
    /// then means "no readable sensors", a legitimate common state), false when the query itself
    /// FAILED (non-zero exit / timeout). Lets a caller distinguish "no sensors" from "could not
    /// query sensors" instead of reporting a failed query as "no thermal data". The no-argument
    /// overload discards this signal (existing callers, e.g. the poll timer / StressTestWorker).
    [[nodiscard]] static QVector<ThermalReading> pollOnce(bool& query_ok);

    /// @brief Query CPU thermal zone temperature from WMI
    /// @return Temperature in Celsius, or -1.0 if unavailable
    /// @note Shared utility -- also used by StressTestWorker
    [[nodiscard]] static double queryCpuTemperature();

    /// @brief The sensor statuses in @p statuses not yet in @p already_logged, recorded there.
    /// A missing sensor reports the same status on every poll; the periodic monitor logs each
    /// distinct status once instead of a warning per sensor every few seconds. Pure; testable.
    [[nodiscard]] static QStringList newSensorStatuses(const QStringList& statuses,
                                                       QSet<QString>* already_logged);

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
    friend class ::ThermalMonitorTests;

    /// @brief Build the combined PowerShell script that queries all sensors
    /// @return PowerShell script string
    [[nodiscard]] static QString buildCombinedThermalScript();

    /// @brief One synchronous sensor query: readings, the unavailable-sensor status lines, and
    ///        whether the query process itself succeeded. Logs nothing about sensors; callers
    ///        decide (the periodic monitor de-duplicates, one-shot pollOnce logs them).
    [[nodiscard]] static ThermalPollResult querySensors();

    /// @brief Parse key=value output from the combined thermal script
    /// @param output Raw stdout from PowerShell
    /// @return Parsed thermal readings
    [[nodiscard]] static QVector<ThermalReading> parseThermalOutput(const QString& output);

    /// @brief Process completed readings: thresholds, signals
    /// @note Readings are NOT retained. This monitor is a pure emitter. It previously kept
    ///       a capped history that no production code ever read - DiagnosticController
    ///       forwards readingsUpdated straight to the panel, which overwrites its labels
    ///       each poll - so a running monitor accumulated up to 1800 readings for nobody.
    ///       A consumer that needs history must keep its own.
    void processReadings(const QVector<ThermalReading>& readings);

    QTimer m_timer;
    QFutureWatcher<ThermalPollResult> m_poll_watcher;
    int m_interval_ms{kDefaultThermalPollIntervalMs};  ///< Configured poll interval
    QSet<QString> m_logged_sensor_statuses;            ///< Statuses already logged once
    bool m_query_failure_logged{false};                ///< Logged until a query succeeds
    bool m_active{
        false};  ///< true between start() and stop(); gates timer re-arm in onPollComplete

    /// Warning thresholds (Celsius)
    static constexpr double kCpuWarningThreshold = 85.0;
    static constexpr double kGpuWarningThreshold = 90.0;
    static constexpr double kDiskWarningThreshold = 55.0;
};

}  // namespace sak
