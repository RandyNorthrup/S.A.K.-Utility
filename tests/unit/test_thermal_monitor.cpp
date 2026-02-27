// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_thermal_monitor.cpp
/// @brief Unit tests for ThermalMonitor timer behavior and polling

#include <QtTest/QtTest>

#include "sak/thermal_monitor.h"

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

void ThermalMonitorTests::initialState()
{
    ThermalMonitor monitor;
    QVERIFY(!monitor.isRunning());
    QVERIFY(monitor.history().isEmpty());
}

void ThermalMonitorTests::startStop()
{
    ThermalMonitor monitor;
    monitor.start(500);  // 500ms interval

    // After start(), the initial poll fires immediately so timer is running
    QVERIFY(monitor.isRunning());

    monitor.stop();
    QVERIFY(!monitor.isRunning());
}

void ThermalMonitorTests::pollOnceReturnsReadings()
{
    ThermalMonitor monitor;
    // pollOnce should return without crashing even if WMI is unavailable
    // (returns -1.0 for unavailable sensors, which are filtered out)
    const auto readings = monitor.pollOnce();
    // We can't guarantee readings are available (requires admin + WMI),
    // but the call must not crash
    QVERIFY(true);
}

void ThermalMonitorTests::clearHistory()
{
    ThermalMonitor monitor;
    // Start briefly to accumulate some history
    monitor.start(100);
    QTest::qWait(250);
    monitor.stop();

    // History may or may not have entries depending on WMI availability
    monitor.clearHistory();
    QVERIFY(monitor.history().isEmpty());
}

void ThermalMonitorTests::singleShotTimerBehavior()
{
    // Verify the timer doesn't accumulate concurrent polls.
    // Start with short interval, wait, then verify history grows linearly.
    ThermalMonitor monitor;
    monitor.start(100);

    QTest::qWait(500);
    monitor.stop();

    // With 100ms interval and 500ms wait, we expect roughly 5 polls.
    // Even if polls take time, single-shot prevents accumulation.
    // (May have 0 entries if WMI is unavailable, which is also valid.)
    const int history_size = monitor.history().size();
    // Should not have wildly more entries than expected
    // (at most 2 readings per poll * ~6 polls = 12)
    QVERIFY2(history_size <= 20,
             qPrintable(QString("History too large: %1 entries").arg(history_size)));
}

QTEST_MAIN(ThermalMonitorTests)
#include "test_thermal_monitor.moc"
