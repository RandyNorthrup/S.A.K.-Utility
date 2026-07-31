// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_check_disk_errors_action.cpp
/// @brief Unit tests for CheckDiskErrorsAction pure seams (Codex B6-09/10):
///        the check must never schedule an offline (mutating) repair, and the
///        overall outcome must reflect every drive, not just one that succeeded.

#include "sak/actions/check_disk_errors_action.h"

#include <QtTest/QtTest>

using sak::CheckDiskErrorsAction;

class CheckDiskErrorsActionTests : public QObject {
    Q_OBJECT

    using Action = CheckDiskErrorsAction;

private Q_SLOTS:
    // B6-09: a "check" must not mutate the disk.
    void buildScanVolumeScript_isReadOnly();
    // B6-10: failed/timed-out drives must affect the outcome.
    void evaluateDiskCheckOutcome_aggregatesAllDrives();
};

void CheckDiskErrorsActionTests::buildScanVolumeScript_isReadOnly() {
    const QString script = Action::buildScanVolumeScript(QChar('C'));

    // Never schedules a boot-time offline repair (the mutation being removed).
    QVERIFY2(!script.contains(QStringLiteral("OfflineScanAndFix")),
             "check script must not schedule an offline repair");
    QVERIFY(!script.contains(QStringLiteral("RebootRequired: Yes")));
    // Still performs the read-only online scan and reports a recommendation.
    QVERIFY(script.contains(QStringLiteral("Repair-Volume -DriveLetter C -Scan")));
    QVERIFY(script.contains(QStringLiteral("RepairRecommended")));
}

void CheckDiskErrorsActionTests::evaluateDiskCheckOutcome_aggregatesAllDrives() {
    // All drives scanned -> success, none failed.
    auto all_ok = Action::evaluateDiskCheckOutcome(3, 3);
    QVERIFY(all_ok.success);
    QCOMPARE(all_ok.drives_failed, 0);

    // One of three scanned -> NOT success; two counted as failed.
    auto partial = Action::evaluateDiskCheckOutcome(1, 3);
    QVERIFY(!partial.success);
    QCOMPARE(partial.drives_failed, 2);

    // No drives at all -> not success, zero failed.
    auto none = Action::evaluateDiskCheckOutcome(0, 0);
    QVERIFY(!none.success);
    QCOMPARE(none.drives_failed, 0);

    // Zero of two scanned -> not success, both failed.
    auto zero = Action::evaluateDiskCheckOutcome(0, 2);
    QVERIFY(!zero.success);
    QCOMPARE(zero.drives_failed, 2);
}

QTEST_GUILESS_MAIN(CheckDiskErrorsActionTests)
#include "test_check_disk_errors_action.moc"
