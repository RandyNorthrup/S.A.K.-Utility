// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevation_manager.cpp
/// @brief Unit tests for ElevationManager â€” non-UAC-triggering operations only

#include "sak/elevation_manager.h"

#include <QtTest/QtTest>

#ifdef _WIN32

class ElevationManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // isElevated
    void isElevated_returnsConsistently();

    // canElevate
    void canElevate_returnsBoolean();

    // getElevationErrorMessage
    void errorMessage_knownCode();
    void errorMessage_unknownCode();
    void errorMessage_accessDenied();
    void errorMessage_operationCancelled();
};

// ============================================================================
// isElevated
// ============================================================================

void ElevationManagerTests::isElevated_returnsConsistently() {
    bool first = sak::ElevationManager::isElevated();
    bool second = sak::ElevationManager::isElevated();
    QCOMPARE(first, second);
}

// ============================================================================
// canElevate
// ============================================================================

void ElevationManagerTests::canElevate_returnsBoolean() {
    bool result = sak::ElevationManager::canElevate();
    Q_UNUSED(result);
    QVERIFY(true);  // Just verify no crash
}

// ============================================================================
// getElevationErrorMessage
// ============================================================================

void ElevationManagerTests::errorMessage_knownCode() {
    // ERROR_ACCESS_DENIED = 5
    QString msg = QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(5));
    QVERIFY(!msg.isEmpty());
}

void ElevationManagerTests::errorMessage_unknownCode() {
    // A very unlikely error code
    QString msg =
        QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(99'999'999));
    QVERIFY(!msg.isEmpty());  // Should have fallback format
}

void ElevationManagerTests::errorMessage_accessDenied() {
    // ERROR_ACCESS_DENIED = 5
    QString msg = QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(5));
    // On Windows, this should produce a meaningful message
    QVERIFY(msg.length() > 5);
}

void ElevationManagerTests::errorMessage_operationCancelled() {
    // ERROR_CANCELLED = 1223
    QString msg = QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(1223));
    QVERIFY(!msg.isEmpty());
}

#else
// Empty test class for non-Windows platforms
class ElevationManagerTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void isElevated_returnsConsistently() { QSKIP("ElevationManager is Windows-only"); }
    void canElevate_returnsBoolean() { QSKIP("ElevationManager is Windows-only"); }
    void errorMessage_knownCode() { QSKIP("ElevationManager is Windows-only"); }
    void errorMessage_unknownCode() { QSKIP("ElevationManager is Windows-only"); }
    void errorMessage_accessDenied() { QSKIP("ElevationManager is Windows-only"); }
    void errorMessage_operationCancelled() { QSKIP("ElevationManager is Windows-only"); }
};
#endif

QTEST_GUILESS_MAIN(ElevationManagerTests)
#include "test_elevation_manager.moc"
