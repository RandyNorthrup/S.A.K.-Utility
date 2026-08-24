// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_thermal_monitor.cpp
/// @brief Unit tests for ThermalMonitor timer behavior and polling

#include "sak/thermal_monitor.h"

#include <QtTest/QtTest>

using namespace sak;

class ThermalMonitorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initialState();
    void startStop();
    void startFiresInitialPollImmediately();
    void pollOnceReadingsAreWellFormed();
    void singleShotTimerBehavior();
    void clampPollInterval_flooring();
};

void ThermalMonitorTests::initialState() {
    ThermalMonitor monitor;
    QVERIFY(!monitor.isRunning());
}

void ThermalMonitorTests::startStop() {
    // Counter declared before the monitor so it outlives the connection.
    int updates = 0;
    ThermalMonitor monitor;
    QObject::connect(&monitor,
                     &ThermalMonitor::readingsUpdated,
                     &monitor,
                     [&updates](const QVector<sak::ThermalReading>&) { ++updates; });

    monitor.start(500);  // 500ms interval

    // After start(), the initial async poll launches immediately
    QVERIFY(monitor.isRunning());

    // Stop while that first poll is still in flight. During a poll the timer is NOT
    // active, so the stop intent survives only through m_active; and nothing spins the
    // event loop between start() and stop(), so the watcher's finished callout (and
    // therefore onPollComplete) cannot have run yet.
    monitor.stop();

    // The in-flight poll drains and NOTHING re-arms the timer: without the m_active
    // guard in onPollComplete the stale result would be processed and the timer
    // restarted, leaving isRunning() true forever and readings arriving after stop().
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.isRunning(), 30'000);
    QTest::qWait(250);     // let any queued watcher callout be delivered
    QVERIFY(!monitor.isRunning());
    QCOMPARE(updates, 0);  // the result of a poll that outlived stop() is dropped
}

// start() fires the initial poll IMMEDIATELY rather than merely arming the timer. The
// stop-during-poll test above depends on that -- it needs an in-flight poll for stop() to race,
// or the m_active guard it is named for is never exercised -- but it cannot prove it: a start()
// that only armed the timer satisfies its isRunning() check just as well.
//
// Proved here without racing the poll: with a 30s interval, a reading can only arrive quickly if
// start() polled at once. If it merely armed the timer, nothing can arrive for 30 seconds.
void ThermalMonitorTests::startFiresInitialPollImmediately() {
    int cycles = 0;
    ThermalMonitor monitor;
    QObject::connect(&monitor,
                     &ThermalMonitor::readingsUpdated,
                     &monitor,
                     [&cycles](const QVector<sak::ThermalReading>&) { ++cycles; });

    monitor.start(30'000);
    QTRY_VERIFY_WITH_TIMEOUT(cycles >= 1, 20'000);
    monitor.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.isRunning(), 30'000);
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
    // Verify the timer doesn't accumulate concurrent polls.
    // With async polling, each cycle takes several seconds (PS startup + WMI),
    // so we allow generous time and just verify no runaway accumulation.
    int cycles = 0;
    ThermalMonitor monitor;
    QObject::connect(&monitor,
                     &ThermalMonitor::readingsUpdated,
                     &monitor,
                     [&cycles](const QVector<sak::ThermalReading>&) { ++cycles; });
    monitor.start(500);

    // processReadings emits readingsUpdated for EVERY completed cycle -- with an empty
    // reading vector on a box whose sensors need admin -- so this counts poll CYCLES on
    // any machine. Bounded by the sensor-query timeout, so it always arrives.
    QTRY_VERIFY_WITH_TIMEOUT(cycles >= 1, 30'000);

    // A completed cycle must leave the monitor ARMED for the next one: onPollComplete re-arms
    // the single-shot timer in the same slot invocation that emitted the readings. Drop that
    // re-arm and the monitor emits exactly one reading set and goes silent -- and the runaway
    // bound below then passes VACUOUSLY with window_cycles == 0.
    QTRY_VERIFY_WITH_TIMEOUT(monitor.isRunning(), 5000);

    const int cycles_before_window = cycles;
    QTest::qWait(3000);
    monitor.stop();

    // Polls are strictly serialized: the timer is single-shot and re-armed only after a
    // poll completes, and onTimerTick defers while one is in flight. A 3s window at a
    // 500ms interval can therefore retire only a handful of cycles even if every poll
    // returned instantly; a timer that stacked a poll per tick would blow past this.
    const int window_cycles = cycles - cycles_before_window;
    QVERIFY2(window_cycles <= 12,
             qPrintable(QString("Runaway poll cycles: %1").arg(window_cycles)));
}

// B5 tail: a non-positive poll interval must never arm the timer (0 busy-spins,
// negative is invalid). start() clamps through clampPollIntervalMs.
void ThermalMonitorTests::clampPollInterval_flooring() {
    // Every assertion below is written RELATIVE to the symbol, so changing the floor's
    // magnitude keeps them all true while the anti-busy-spin guarantee moves. Pin the value.
    QCOMPARE(sak::kMinThermalPollIntervalMs, 250);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(1000), 1000);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(sak::kMinThermalPollIntervalMs),
             sak::kMinThermalPollIntervalMs);
    // Below the floor / zero / negative all clamp up to the floor.
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(10), sak::kMinThermalPollIntervalMs);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(0), sak::kMinThermalPollIntervalMs);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(-5), sak::kMinThermalPollIntervalMs);
}

QTEST_MAIN(ThermalMonitorTests)
#include "test_thermal_monitor.moc"
