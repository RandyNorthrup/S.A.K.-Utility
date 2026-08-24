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

    // Real DISM /CheckHealth prints a version banner BEFORE the verdict, so the healthy line is
    // not at offset 0: the exclusion must be a whole-string search, never an anchored
    // startsWith()/^regex. (B6-07: anchoring flips every clean machine to "corrupt" and forces a
    // needless /RestoreHealth.)
    QVERIFY(!Action::dismReportsCorruption(
        QStringLiteral("Deployment Image Servicing and Management tool\n"
                       "Version: 10.0.19041.844\n"
                       "\n"
                       "Image Version: 10.0.19041.844\n"
                       "\n"
                       "No component store corruption detected.\n"
                       "The operation completed successfully.")));
    // The exclusion is case-insensitive BY CONTRACT: a differently-cased healthy verdict must
    // still read as clean. Dropping Qt::CaseInsensitive from the exclusion would let the bare
    // "corruption" arm -- itself case-insensitive -- fire on this healthy output.
    QVERIFY(
        !Action::dismReportsCorruption(QStringLiteral("no component store CORRUPTION detected.\n"
                                                      "The operation completed successfully.")));
}

void VerifySystemFilesActionTests::dismReportsCorruption_repairableAndDetectedAreCorrupt() {
    // Repairable corruption (CheckHealth/ScanHealth phrasing).
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("The component store is repairable.\n"
                                                     "The operation completed successfully.")));
    // Explicit corruption-detected phrasing (not the "No ... corruption" verdict).
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("Component store corruption was detected.")));
    // The exclusion is the FULL "No component store corruption" verdict, not the
    // "corruption detected" tail it shares with the corrupt phrasing: the same tail without the
    // leading "No" is corruption.
    QVERIFY(
        Action::dismReportsCorruption(QStringLiteral("Component store corruption detected.\n"
                                                     "The operation completed successfully.")));
}

void VerifySystemFilesActionTests::makeUniqueSfcOutputName_uniqueAndWellFormed() {
    const QString a = Action::makeUniqueSfcOutputName(1234, 999'999, 0);
    const QString b = Action::makeUniqueSfcOutputName(1234, 999'999, 1);

    QCOMPARE(a, QStringLiteral("sak_sfc_output_1234_999999_0.txt"));
    // Production feeds a 63-bit OS-CSPRNG token and a ~1.76e12 epoch-ms stamp. Pin those at full
    // width: any narrowing of the leading field (int truncation, a "keep the name short" modulo)
    // would collapse the unpredictability that makes a planted-reparse-point hijack of the
    // elevated SFC write impractical, and small toy inputs render byte-identically under such a
    // bug.
    QCOMPARE(Action::makeUniqueSfcOutputName(
                 0x7F'FF'FF'FF'FF'FF'FF'FFLL, 1'766'000'000'000LL, 4'294'967'295u),
             QStringLiteral("sak_sfc_output_9223372036854775807_1766000000000_4294967295.txt"));
    // Every counter value is rendered VERBATIM as its own field -- it is not folded
    // into a tie-break bit -- so N runs inside one millisecond yield N distinct
    // redirect targets (verify_system_files_action.cpp:61,71).
    QCOMPARE(b, QStringLiteral("sak_sfc_output_1234_999999_1.txt"));
    QCOMPARE(Action::makeUniqueSfcOutputName(1234, 999'999, 2),
             QStringLiteral("sak_sfc_output_1234_999999_2.txt"));
    QCOMPARE(Action::makeUniqueSfcOutputName(1234, 999'999, 4'294'967'295u),
             QStringLiteral("sak_sfc_output_1234_999999_4294967295.txt"));
    QVERIFY(a != b);  // same pid+ms, different counter -> distinct
    QVERIFY(a.endsWith(QStringLiteral(".txt")));
    QVERIFY(a != b);  // same pid+ms, different counter -> distinct
    QVERIFY(a.endsWith(QStringLiteral(".txt")));
    // Must be safe to embed in a single-quoted PowerShell string.
    QVERIFY(!a.contains(QLatin1Char('\'')));
    QVERIFY(!a.contains(QLatin1Char(' ')));
}

QTEST_GUILESS_MAIN(VerifySystemFilesActionTests)
#include "test_verify_system_files_action.moc"
