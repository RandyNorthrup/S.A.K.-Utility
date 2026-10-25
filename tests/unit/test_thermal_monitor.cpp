// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_thermal_monitor.cpp
/// @brief Unit tests for ThermalMonitor timer behavior and polling

#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/thermal_monitor.h"

#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <filesystem>
#include <optional>

using namespace sak;

namespace {
ProcessResult successfulSensorQuery() {
    ProcessResult result;
    result.exit_code = 0;
    result.std_out = QStringLiteral("cpu=42.0");
    return result;
}
}  // namespace

class ThermalMonitorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initialState();
    void startStop();
    void startFiresInitialPollImmediately();
    void pollOnceReadingsAreWellFormed();
    void sensorScriptSurfacesUnavailableQueries();
    void singleShotTimerBehavior();
    void clampPollInterval_flooring();
    void missingSensorIsLoggedOnce();
    void missingSensorWarnsOnceAcrossPolls();

private:
    // Outlives every test: the logger keeps writing into it once initialized.
    QTemporaryDir m_log_dir;
};

void ThermalMonitorTests::sensorScriptSurfacesUnavailableQueries() {
    const QString script = ThermalMonitor::buildCombinedThermalScript();
    QVERIFY2(!script.contains(QStringLiteral("SilentlyContinue")), qPrintable(script));
    QVERIFY2(!script.contains(QStringLiteral("2>$null")), qPrintable(script));
    QVERIFY2(!script.contains(QStringLiteral("catch{}")), qPrintable(script));
    QVERIFY(script.contains(QStringLiteral("status_cpu=unavailable:")));
    QVERIFY(script.contains(QStringLiteral("status_gpu=unavailable:")));
    QVERIFY(script.contains(QStringLiteral("status_disk=unavailable:")));
    QVERIFY(script.contains(QStringLiteral("-ErrorAction Stop")));
}

void ThermalMonitorTests::initialState() {
    ThermalMonitor monitor;
    QVERIFY(!monitor.isRunning());
}

void ThermalMonitorTests::startStop() {
    // Hold the first query in flight so stop() always races the poll, regardless of
    // PowerShell startup time or whether this host has readable sensors.
    QSemaphore poll_entered;
    QSemaphore finish_poll;
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            poll_entered.release();
            finish_poll.acquire();
            return successfulSensorQuery();
        });

    int updates = 0;
    ThermalMonitor monitor;
    const auto unblock_poll = qScopeGuard([&] {
        finish_poll.release();
        monitor.stop();
    });
    QObject::connect(&monitor,
                     &ThermalMonitor::readingsUpdated,
                     &monitor,
                     [&updates](const QVector<sak::ThermalReading>&) { ++updates; });
    QSignalSpy finished_spy(&monitor.m_poll_watcher,
                            &QFutureWatcher<sak::ThermalPollResult>::finished);
    QVERIFY(finished_spy.isValid());

    monitor.start(500);  // 500ms interval
    QVERIFY2(poll_entered.tryAcquire(1, 5000), "Initial sensor query did not start");
    QVERIFY(monitor.isRunning());

    // During the held poll the one-shot timer is inactive. Only m_active carries
    // stop intent into onPollComplete().
    QVERIFY(!monitor.m_timer.isActive());
    monitor.stop();
    finish_poll.release();
    QTRY_VERIFY_WITH_TIMEOUT(finished_spy.count() >= 1, 5000);
    QVERIFY(!monitor.isRunning());
    QCOMPARE(updates, 0);  // result of a poll that outlived stop() is dropped
}

// start() fires the initial poll IMMEDIATELY rather than merely arming the timer. The
// stop-during-poll test above depends on that -- it needs an in-flight poll for stop() to race,
// or the m_active guard it is named for is never exercised -- but it cannot prove it: a start()
// that only armed the timer satisfies its isRunning() check just as well.
//
// Proved here without racing the poll: with a 30s interval, a reading can only arrive quickly if
// start() polled at once. If it merely armed the timer, nothing can arrive for 30 seconds.
void ThermalMonitorTests::startFiresInitialPollImmediately() {
    std::atomic<int> queries{0};
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            queries.fetch_add(1);
            return successfulSensorQuery();
        });

    int cycles = 0;
    ThermalMonitor monitor;
    QObject::connect(&monitor,
                     &ThermalMonitor::readingsUpdated,
                     &monitor,
                     [&cycles](const QVector<sak::ThermalReading>&) { ++cycles; });

    monitor.start(30'000);
    QTRY_VERIFY_WITH_TIMEOUT(cycles >= 1, 5000);
    QCOMPARE(queries.load(), 1);
    monitor.stop();
    QVERIFY(!monitor.isRunning());
}

void ThermalMonitorTests::pollOnceReadingsAreWellFormed() {
    // A live sensor query: MSAcpi_ThermalZoneTemperature and StorageReliabilityCounter
    // both need admin, so an EMPTY result is legitimate and the reading COUNT cannot be
    // asserted here. Two things are invariant on any box: a query that FAILED
    // (unresolvable interpreter, non-zero exit, timeout) must report no readings at all,
    // and every reading that IS returned must have survived parseThermalOutput's filters.
    bool queryOk = false;
    const auto readings = ThermalMonitor::pollOnce(queryOk);
    QVERIFY2(queryOk || readings.isEmpty(), "a failed sensor query must not yield readings");

    for (const auto& reading : readings) {
        const bool known_component = reading.component == QStringLiteral("CPU Package") ||
                                     reading.component == QStringLiteral("GPU") ||
                                     reading.component.startsWith(QStringLiteral("Disk "));
        QVERIFY2(known_component, qPrintable("Unexpected component: " + reading.component));
        QVERIFY2(reading.temperature_celsius > 0.0,
                 qPrintable(QString("Non-positive temperature for %1").arg(reading.component)));
        QVERIFY2(reading.timestamp.isValid(), qPrintable("No timestamp on " + reading.component));
    }
}

void ThermalMonitorTests::singleShotTimerBehavior() {
    // Exercise the real timer, watcher and result path with a controlled process result.
    // Poll time is fixed by this fixture, not by host PowerShell/WMI scheduling.
    std::atomic<int> active_queries{0};
    std::atomic<int> peak_queries{0};
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            const int concurrent = active_queries.fetch_add(1) + 1;
            int peak = peak_queries.load();
            while (concurrent > peak && !peak_queries.compare_exchange_weak(peak, concurrent)) {}
            QThread::msleep(150);
            active_queries.fetch_sub(1);
            return successfulSensorQuery();
        });

    ThermalMonitor monitor;
    QSignalSpy readings_spy(&monitor, &ThermalMonitor::readingsUpdated);
    QVERIFY(readings_spy.isValid());
    monitor.start(250);
    QVERIFY(monitor.m_timer.isSingleShot());

    // Lower bound is eventual rather than a fixed 3s window. Removing the
    // onPollComplete timer re-arm leaves exactly one cycle and fails this check.
    QTRY_VERIFY_WITH_TIMEOUT(readings_spy.count() >= 3, 10'000);
    QCOMPARE(monitor.m_timer.interval(), 250);
    QCOMPARE(peak_queries.load(), 1);

    const int cycles_before_window = readings_spy.count();
    QTest::qWait(1000);
    monitor.stop();

    // A 1s window with 250ms cadence cannot accumulate unbounded cycles. The
    // separate three-cycle lower bound above keeps this upper bound non-vacuous.
    const int window_cycles = readings_spy.count() - cycles_before_window;
    QVERIFY2(window_cycles <= 5, qPrintable(QString("Runaway poll cycles: %1").arg(window_cycles)));
    QCOMPARE(peak_queries.load(), 1);
}

// B5 tail: a non-positive poll interval must never arm the timer (0 busy-spins,
// negative is invalid). start() clamps through clampPollIntervalMs.
void ThermalMonitorTests::clampPollInterval_flooring() {
    // Every assertion below is written RELATIVE to the symbol, so changing the floor's
    // magnitude keeps them all true while the anti-busy-spin guarantee moves. Pin the value.
    QCOMPARE(sak::kMinThermalPollIntervalMs, 250);
    // The DEFAULT cadence has its own constant (it used to borrow the peer-discovery broadcast
    // interval, 2 s). Each poll launches a PowerShell process, so a glanceable readout polls
    // no faster than every 5 s. Pin the value and pin that it sits ABOVE the floor.
    QCOMPARE(sak::kDefaultThermalPollIntervalMs, sak::kThermalReadoutPollIntervalMs);
    QCOMPARE(sak::kThermalReadoutPollIntervalMs, 5000);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(sak::kDefaultThermalPollIntervalMs),
             sak::kThermalReadoutPollIntervalMs);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(1000), 1000);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(sak::kMinThermalPollIntervalMs),
             sak::kMinThermalPollIntervalMs);
    // Below the floor / zero / negative all clamp up to the floor.
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(10), sak::kMinThermalPollIntervalMs);
    // Both sides of the EXACT boundary. Without them a floor written against the wrong
    // constant -- `requested >= 100 ? requested : kMinThermalPollIntervalMs` -- satisfies
    // every other case here (1000, 250, 10, 0, -5 all stay green) while start(150) arms a
    // 150ms sensor poll, killing the contract at thermal_monitor.h:64-67. A `>= 500` variant
    // passes them too, and dies on the 251 probe.
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(249), 250);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(251), 251);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(0), sak::kMinThermalPollIntervalMs);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(-5), sak::kMinThermalPollIntervalMs);
}

// A sensor that is not there reports the same status line on every poll. The monitor used to
// log a warning per missing sensor per poll -- every 2 s for the life of the app. Each distinct
// status is now logged once; a new status (another sensor, another reason) still is.
void ThermalMonitorTests::missingSensorIsLoggedOnce() {
    QSet<QString> logged;
    const QStringList first = {QStringLiteral("status_gpu=unavailable:command_not_supported"),
                               QStringLiteral("status_disk=unavailable:no_devices")};
    QCOMPARE(ThermalMonitor::newSensorStatuses(first, &logged), first);
    QCOMPARE(ThermalMonitor::newSensorStatuses(first, &logged), QStringList());
    const QStringList changed = {QStringLiteral("status_gpu=unavailable:command_not_supported"),
                                 QStringLiteral("status_cpu=unavailable:no_reading")};
    QCOMPARE(ThermalMonitor::newSensorStatuses(changed, &logged),
             QStringList{QStringLiteral("status_cpu=unavailable:no_reading")});
    QCOMPARE(logged.size(), 3);
}

// The same, end to end: several real poll cycles that each report the missing GPU leave one
// warning in the application log, not one per poll.
void ThermalMonitorTests::missingSensorWarnsOnceAcrossPolls() {
    QVERIFY(m_log_dir.isValid());
    auto& log = logger::instance();
    QVERIFY(log.initialize(std::filesystem::path(m_log_dir.path().toStdWString()), "thermal")
                .has_value());
    const QString missing = QStringLiteral("status_gpu=unavailable:command_not_supported");
    std::atomic<int> queries{0};
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            queries.fetch_add(1);
            ProcessResult result = successfulSensorQuery();
            result.std_out += QStringLiteral("\n") + missing;
            return result;
        });

    ThermalMonitor monitor;
    QSignalSpy readings_spy(&monitor, &ThermalMonitor::readingsUpdated);
    monitor.start(kMinThermalPollIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(readings_spy.count() >= 3, 10'000);
    monitor.stop();
    log.flush();

    QFile file(QString::fromStdWString(log.getLogFile().wstring()));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
    const QString text = QString::fromUtf8(file.readAll());
    QVERIFY2(queries.load() >= 3, "three polls ran");
    QCOMPARE(text.count(missing), 1);
}

QTEST_MAIN(ThermalMonitorTests)
#include "test_thermal_monitor.moc"
