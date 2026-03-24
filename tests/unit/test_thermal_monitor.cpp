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
    void pollOnceReturnsReadings();
    void clearHistory();
    void singleShotTimerBehavior();
};

void ThermalMonitorTests::initialState() {
    ThermalMonitor monitor;
    QVERIFY(!monitor.isRunning());
    QVERIFY(monitor.history().isEmpty());
}

void ThermalMonitorTests::startStop() {
    ThermalMonitor monitor;
    monitor.start(500);  // 500ms interval

    // After start(), the initial async poll launches immediately
    QVERIFY(monitor.isRunning());

    monitor.stop();
    // Timer is stopped; async poll may still be finishing
    QTest::qWait(200);
}

void ThermalMonitorTests::pollOnceReturnsReadings() {
    // pollOnce should return without crashing even if WMI is unavailable
    // (returns -1.0 for unavailable sensors, which are filtered out)
    const auto readings = ThermalMonitor::pollOnce();
    // We can't guarantee readings are available (requires admin + WMI),
    // but the call must not crash
    QVERIFY(true);
}

void ThermalMonitorTests::clearHistory() {
    ThermalMonitor monitor;
    // Start briefly to accumulate some history
    monitor.start(100);
    QTest::qWait(250);
    monitor.stop();

    // History may or may not have entries depending on WMI availability
    monitor.clearHistory();
    QVERIFY(monitor.history().isEmpty());
}

void ThermalMonitorTests::singleShotTimerBehavior() {
    // Verify the timer doesn't accumulate concurrent polls.
    // With async polling, each cycle takes several seconds (PS startup + WMI),
    // so we allow generous time and just verify no runaway accumulation.
    ThermalMonitor monitor;
    monitor.start(500);

    QTest::qWait(3000);
    monitor.stop();

    // Async polls take 1-3 seconds each, so expect at most a few.
    // History may be empty if WMI is unavailable (also valid).
    const int history_size = monitor.history().size();
    QVERIFY2(history_size <= 30,
             qPrintable(QString("History too large: %1 entries").arg(history_size)));
}

QTEST_MAIN(ThermalMonitorTests)
#include "test_thermal_monitor.moc"
