// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_panel.cpp
/// @brief The Network panel's start-up adapter scan runs, off-thread, in every run mode.

#include "sak/network_diagnostic_controller.h"
#include "sak/network_diagnostic_panel.h"

#include <QApplication>
#include <QtTest/QtTest>

#include <memory>

namespace {
// One read-only adapter enumeration (GetAdaptersAddresses plus the per-adapter details).
constexpr int kAdapterScanTimeoutMs = 30'000;
}  // namespace

class NetworkDiagnosticPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();
    void adapterScanRunsInEveryMode_data();
    void adapterScanRunsInEveryMode();
    void teardownRightAfterConstructionJoinsTheScan();
};

void NetworkDiagnosticPanelTests::cleanup() {
    qApp->setProperty("sakStartupSmokeTest", QVariant());
    qApp->setProperty("sakAccessibilityAudit", QVariant());
}

void NetworkDiagnosticPanelTests::adapterScanRunsInEveryMode_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::newRow("interactive") << QByteArray();
    QTest::newRow("startup smoke") << QByteArray("sakStartupSmokeTest");
    QTest::newRow("accessibility audit") << QByteArray("sakAccessibilityAudit");
}

// Smoke and audit runs used to skip the adapter scan users get when the panel is created, so the
// gates that exist to exercise start-up work never ran it. It now runs in every mode, on the
// controller's worker thread.
void NetworkDiagnosticPanelTests::adapterScanRunsInEveryMode() {
    QFETCH(QByteArray, mode);
    if (!mode.isEmpty()) {
        qApp->setProperty(mode.constData(), true);
    }
    sak::NetworkDiagnosticPanel panel;
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);
    int scans = 0;
    QObject::connect(controller,
                     &sak::NetworkDiagnosticController::adaptersScanComplete,
                     &panel,
                     [&scans](const QVector<sak::NetworkAdapterInfo>&) { ++scans; });
    QTRY_VERIFY_WITH_TIMEOUT(scans >= 1, kAdapterScanTimeoutMs);
}

// What the old skip guarded against: a gate tears the panel down moments after building it,
// with the scan still running. The controller's destructor joins its worker, so this returns
// cleanly instead of racing a scan into freed state.
void NetworkDiagnosticPanelTests::teardownRightAfterConstructionJoinsTheScan() {
    qApp->setProperty("sakStartupSmokeTest", true);
    for (int round = 0; round < 3; ++round) {
        auto panel = std::make_unique<sak::NetworkDiagnosticPanel>();
        QCoreApplication::processEvents();  // dispatch the queued scan
        panel.reset();
    }
}

QTEST_MAIN(NetworkDiagnosticPanelTests)
#include "test_network_diagnostic_panel.moc"
