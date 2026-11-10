// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.h
/// @brief Real-time thermal sensor monitoring

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/layout_constants.h"

#include <QFutureWatcher>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>

class ThermalMonitorTests;

namespace sak {

inline constexpr int kDefaultThermalPollIntervalMs = kThermalReadoutPollIntervalMs;

/// @brief One periodic poll's outcome, carried from the pool thread back to the monitor.
struct ThermalPollResult {
    QVector<ThermalReading> readings;
    /// "status_<sensor>=unavailable:<reason>" lines for sensors the script could not read.
    QStringList sensor_statuses;
    /// Each disk's identity by its disk number, from "identity_disk<n>=<id>": its unique ID, or
    /// its serial number when Windows gives no unique ID.
    QMap<QString, QString> disk_identities;
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
    /// @return true from start() until stop(), and after stop() until its last poll has landed
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
    /// @param should_cancel Asked while the query runs; true stops it and answers -1.0.
    /// @return Temperature in Celsius, or -1.0 if unavailable
    /// @note Shared utility -- also used by StressTestWorker
    [[nodiscard]] static double queryCpuTemperature(
        const std::function<bool()>& should_cancel = {});

    /// @brief The sensor statuses in @p statuses not yet in @p already_logged, recorded there.
    /// A missing sensor reports the same status on every poll; the periodic monitor logs each
    /// distinct status once instead of a warning per sensor every few seconds. Pure; testable.
    [[nodiscard]] static QStringList newSensorStatuses(const QStringList& statuses,
                                                       QSet<QString>* already_logged);

    /// @brief Why @p sensor ("cpu", "gpu" or "disk") gave no reading, from one poll's
    ///        "status_<sensor>=unavailable:<code>" lines: "needs administrator rights", "not
    ///        reported by this PC", "did not answer in time", or "could not be read". A bare N/A
    ///        told a technician nothing.
    [[nodiscard]] static QString unavailableReason(const QStringList& statuses,
                                                   const QString& sensor);

    /// @brief Ask every disk again, including those that gave no reading. A hardware or SMART
    ///        scan calls this: a drive that was spun down, or just plugged in, may answer now.
    void askEveryDiskAgain();

Q_SIGNALS:
    /// @brief Emitted at each poll interval with current readings
    /// @param readings Current temperature for all detected sensors
    void readingsUpdated(const QVector<sak::ThermalReading>& readings);

    /// @brief Emitted at each poll, before readingsUpdated, with the poll's unavailable-sensor
    ///        status lines (a failed query is "status_query=unavailable:query_failed").
    void sensorStatusesUpdated(const QStringList& statuses);

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

    /// @brief The disks the monitor has stopped asking, by identity, and the disks present when
    ///        it decided. A disk number is not an identity: technicians plug drives in and out,
    ///        and disk 1 can be another drive ten minutes later.
    struct QuietDisks {
        QStringList present;                        ///< identities seen on the last poll, sorted
        QMap<QString, QString> status_by_identity;  ///< identity -> why it gave no reading
    };

    /// @brief Build the combined PowerShell script that queries all sensors
    /// @param quiet_identities Disks not to ask, by identity: an earlier poll had no reading.
    /// @return PowerShell script string
    [[nodiscard]] static QString buildCombinedThermalScript(
        const QStringList& quiet_identities = {});

    /// @brief One synchronous sensor query: readings, the unavailable-sensor status lines, and
    ///        whether the query process itself succeeded. Logs nothing about sensors; callers
    ///        decide (the periodic monitor de-duplicates, one-shot pollOnce logs them).
    [[nodiscard]] static ThermalPollResult querySensors(const QStringList& quiet_identities = {});

    /// @brief Records in @p quiet each disk @p poll says gave no reading, by identity, and
    ///        returns the poll's statuses with the remembered line of each quiet disk it did not
    ///        ask. A disk arriving or leaving forgets them all. A disk that took 12 s to say
    ///        nothing was asked again every 5 s, and kept its USB bridge busy through a disk
    ///        benchmark on it.
    [[nodiscard]] static QStringList rememberQuietDisks(const ThermalPollResult& poll,
                                                        QuietDisks* quiet);

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
    QuietDisks m_quiet_disks;                          ///< Disks not asked again, and why
    bool m_query_failure_logged{false};                ///< Logged until a query succeeds
    bool m_active{
        false};  ///< true between start() and stop(); gates timer re-arm in onPollComplete

    /// Warning thresholds (Celsius)
    static constexpr double kCpuWarningThreshold = 85.0;
    static constexpr double kGpuWarningThreshold = 90.0;
    static constexpr double kDiskWarningThreshold = 55.0;
};

}  // namespace sak
