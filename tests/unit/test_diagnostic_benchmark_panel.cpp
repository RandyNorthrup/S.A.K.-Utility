// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_benchmark_panel.cpp
/// @brief The thermal readout polls only while the Diagnostics panel is shown.

#include "sak/diagnostic_benchmark_panel.h"
#include "sak/thermal_monitor.h"

#include <QApplication>
#include <QtTest/QtTest>

namespace {
// One real sensor query (PowerShell: WMI zone, nvidia-smi, reliability counters) is in flight
// when the panel hides; stopping waits for it to land. Bounded well past its own 10 s timeout.
constexpr int kPollSettleTimeoutMs = 30'000;
}  // namespace

class DiagnosticBenchmarkPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();
    void thermalPollsOnlyWhileShown_data();
    void thermalPollsOnlyWhileShown();
};

void DiagnosticBenchmarkPanelTests::cleanup() {
    qApp->setProperty("sakStartupSmokeTest", QVariant());
    qApp->setProperty("sakAccessibilityAudit", QVariant());
}

void DiagnosticBenchmarkPanelTests::thermalPollsOnlyWhileShown_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::newRow("interactive") << QByteArray();
    QTest::newRow("startup smoke") << QByteArray("sakStartupSmokeTest");
    QTest::newRow("accessibility audit") << QByteArray("sakAccessibilityAudit");
}

// The panel used to start thermal polling in its constructor and keep spawning a PowerShell
// sensor query every 2 s until exit, whether or not the Diagnostics tab was ever looked at --
// except in smoke and audit runs, which skipped it and so never exercised what users got. It
// now polls while shown, stops when hidden, and behaves the same in every run mode.
void DiagnosticBenchmarkPanelTests::thermalPollsOnlyWhileShown() {
    QFETCH(QByteArray, mode);
    if (!mode.isEmpty()) {
        qApp->setProperty(mode.constData(), true);
    }
    sak::DiagnosticBenchmarkPanel panel;
    auto* monitor = panel.findChild<sak::ThermalMonitor*>();
    QVERIFY(monitor != nullptr);
    QVERIFY2(!monitor->isRunning(), "constructing the panel must not start sensor polling");

    panel.show();
    QVERIFY2(monitor->isRunning(), "a shown readout polls");

    panel.hide();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor->isRunning(), kPollSettleTimeoutMs);
    // ...and stays stopped: the in-flight poll landing must not re-arm the timer.
    QTest::qWait(1);
    QVERIFY(!monitor->isRunning());

    panel.show();
    QVERIFY2(monitor->isRunning(), "showing the readout again resumes polling");
    panel.hide();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor->isRunning(), kPollSettleTimeoutMs);
}

QTEST_MAIN(DiagnosticBenchmarkPanelTests)
#include "test_diagnostic_benchmark_panel.moc"
