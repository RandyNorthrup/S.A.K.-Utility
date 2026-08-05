// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevation_manager.cpp
/// @brief Unit tests for ElevationManager -- non-UAC-triggering operations only

#include "sak/elevation_manager.h"

#include <QtTest/QtTest>

#ifdef _WIN32

class ElevationManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // isElevated
    void isElevated_returnsConsistently();

    // canElevate
    void canElevate_reportsUacAvailable();

    // getElevationErrorMessage
    void errorMessage_knownCode();
    void errorMessage_unknownCode();
    void errorMessage_operationCancelled();
};

// ============================================================================
// isElevated
// ============================================================================

void ElevationManagerTests::isElevated_returnsConsistently() {
    // The query must be repeatable: it allocates an Administrators SID and frees it on
    // every call, so a mismatched Free/Allocate (or a cached first answer) shows up as a
    // second call that disagrees with the first.
    bool first = sak::ElevationManager::isElevated();
    bool second = sak::ElevationManager::isElevated();
    QCOMPARE(first, second);
}

// ============================================================================
// canElevate
// ============================================================================

void ElevationManagerTests::canElevate_reportsUacAvailable() {
    // canElevate() is a VerifyVersionInfo(major >= 6) probe, so on every Windows this
    // suite can run on (Vista and later) it MUST report true. The previous body captured
    // the result, Q_UNUSED'd it and ended in QVERIFY(true), so an inverted return or a
    // broken condition mask would have passed unnoticed.
    QVERIFY(sak::ElevationManager::canElevate());
}

// ============================================================================
// getElevationErrorMessage
// ============================================================================

void ElevationManagerTests::errorMessage_knownCode() {
    // ERROR_ACCESS_DENIED = 5 is in the system message table, so FormatMessage must return
    // the SYSTEM text -- not the "Error code: N" fallback -- and the CRLF FormatMessage
    // appends must be stripped. "Non-empty" alone was satisfied by the fallback too.
    const QString msg = QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(5));
    QVERIFY(!msg.isEmpty());
    QVERIFY2(msg != QStringLiteral("Error code: 5"),
             "fell back to the numeric form for a well-known system error code");
    QVERIFY2(!msg.endsWith(QLatin1Char('\n')) && !msg.endsWith(QLatin1Char('\r')),
             qPrintable(QStringLiteral("trailing newline not stripped: %1").arg(msg)));
}

void ElevationManagerTests::errorMessage_unknownCode() {
    // 99999999 has no entry in the system message table, so the documented fallback format
    // is the ONLY correct output. Checking merely "non-empty" also accepted a real system
    // message, i.e. it never proved the fallback branch ran at all.
    const QString msg =
        QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(99'999'999));
    QCOMPARE(msg, QStringLiteral("Error code: 99999999"));
}

void ElevationManagerTests::errorMessage_operationCancelled() {
    // ERROR_CANCELLED = 1223. Distinct known codes must map to DISTINCT system messages;
    // a lookup that ignored its argument would still satisfy a "non-empty" check.
    const QString msg =
        QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(1223));
    QVERIFY(!msg.isEmpty());
    QVERIFY2(msg != QStringLiteral("Error code: 1223"), qPrintable(msg));
    const QString accessDenied =
        QString::fromStdString(sak::ElevationManager::getElevationErrorMessage(5));
    QVERIFY2(msg != accessDenied,
             qPrintable(QStringLiteral("1223 and 5 produced the same message: %1").arg(msg)));
}

// errorMessage_accessDenied was removed here: it called getElevationErrorMessage(5) exactly
// like errorMessage_knownCode above and asserted only length() > 5, a strictly weaker claim
// about the same input. The surviving test now pins the message contents instead.

#else
// Empty test class for non-Windows platforms
class ElevationManagerTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void isElevated_returnsConsistently() { QSKIP("ElevationManager is Windows-only"); }
    void canElevate_reportsUacAvailable() { QSKIP("ElevationManager is Windows-only"); }
    void errorMessage_knownCode() { QSKIP("ElevationManager is Windows-only"); }
    void errorMessage_unknownCode() { QSKIP("ElevationManager is Windows-only"); }
    void errorMessage_operationCancelled() { QSKIP("ElevationManager is Windows-only"); }
};
#endif

QTEST_GUILESS_MAIN(ElevationManagerTests)
#include "test_elevation_manager.moc"
