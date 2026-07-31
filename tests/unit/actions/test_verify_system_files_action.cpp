// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_verify_system_files_action.cpp
/// @brief Unit tests for VerifySystemFilesAction pure seams (Codex B6-07/08):
///        DISM corruption detection must not fire on a clean machine, and the
///        SFC temp filename must be unique per run.

#include "sak/actions/verify_system_files_action.h"

#include <QtTest/QtTest>

using sak::VerifySystemFilesAction;

class VerifySystemFilesActionTests : public QObject {
    Q_OBJECT

    using Action = VerifySystemFilesAction;

private Q_SLOTS:
    // B6-07: the healthy DISM verdict contains the substring "corruption"; it
    // must still be read as "no corruption" so RestoreHealth is not run needlessly.
    void dismReportsCorruption_cleanMachineIsNotCorrupt();
    void dismReportsCorruption_repairableAndDetectedAreCorrupt();
    // B6-08: SFC redirect target is unpredictable and PowerShell-safe.
    void makeUniqueSfcOutputName_uniqueAndWellFormed();
};

void VerifySystemFilesActionTests::dismReportsCorruption_cleanMachineIsNotCorrupt() {
    // The exact DISM /CheckHealth and /ScanHealth healthy output.
    QVERIFY(
        !Action::dismReportsCorruption(QStringLiteral("No component store corruption detected.\n"
                                                      "The operation completed successfully.")));
    // Bare success line with no corruption vocabulary at all.
    QVERIFY(
        !Action::dismReportsCorruption(QStringLiteral("The operation completed successfully.")));
    QVERIFY(!Action::dismReportsCorruption(QString()));
}

void VerifySystemFilesActionTests::dismReportsCorruption_repairableAndDetectedAreCorrupt() {
    // Repairable corruption (CheckHealth/ScanHealth phrasing).
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("The component store is repairable.\n"
                                                     "The operation completed successfully.")));
    // Explicit corruption-detected phrasing (not the "No ... corruption" verdict).
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("Component store corruption was detected.")));
}

void VerifySystemFilesActionTests::makeUniqueSfcOutputName_uniqueAndWellFormed() {
    const QString a = Action::makeUniqueSfcOutputName(1234, 999'999, 0);
    const QString b = Action::makeUniqueSfcOutputName(1234, 999'999, 1);

    QCOMPARE(a, QStringLiteral("sak_sfc_output_1234_999999_0.txt"));
    QVERIFY(a != b);  // same pid+ms, different counter -> distinct
    QVERIFY(a.endsWith(QStringLiteral(".txt")));
    // Must be safe to embed in a single-quoted PowerShell string.
    QVERIFY(!a.contains(QLatin1Char('\'')));
    QVERIFY(!a.contains(QLatin1Char(' ')));
}

QTEST_GUILESS_MAIN(VerifySystemFilesActionTests)
#include "test_verify_system_files_action.moc"
