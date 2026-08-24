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
    // Pin the SOLE Repair-Volume invocation exactly. PowerShell binds any unambiguous prefix
    // (-Offline invokes -OfflineScanAndFix), and an APPENDED second call (-SpotFix) mutates the
    // volume ONLINE without ever spelling "OfflineScanAndFix" -- so the absence assertion above
    // cannot stand alone, and nothing else bounded how many Repair-Volume calls are emitted.
    QCOMPARE(script.count(QStringLiteral("Repair-Volume")), 1);
    QVERIFY2(script.contains(
                 QStringLiteral("Repair-Volume -DriveLetter C -Scan -ErrorAction Stop\n")),
             "the one Repair-Volume call must be exactly the read-only -Scan form");

    // "RebootRequired: Yes" is a string this script never emits in ANY implementation -- a
    // boot-time repair is scheduled by a PARAMETER, not by printing that text -- so that
    // assertion could not see what it claimed to forbid. The tokens that are load-bearing are
    // the ones parseDriveScanResult and processScanKeyValue match on by exact equality; rename
    // any of them and the whole scan pipeline silently degrades with no test noticing.
    QVERIFY2(script.contains(QStringLiteral("Write-Output '===SCAN_START==='")),
             "parseDriveScanResult opens a block only on exact '===SCAN_START==='");
    QVERIFY2(script.contains(QStringLiteral("Write-Output '===SCAN_END==='")),
             "the drive is counted as scanned only on exact '===SCAN_END==='");
    QVERIFY2(script.contains(QStringLiteral("Write-Output 'OnlineScan: Success'")),
             "processScanKeyValue sets scan_success only for the exact value 'Success'");
    QVERIFY2(script.contains(QStringLiteral("Write-Output \"Drive: $drive\"")),
             "the drive letter reaches the report only via the exact 'Drive' key");

    // Both verdict branches, bound to their own arm by relative order: repairs_recommended
    // increments only on the exact value 'Yes'.
    const qsizetype detected_at =
        script.indexOf(QStringLiteral("Write-Output 'CorruptFile: Detected'"));
    const qsizetype yes_at =
        script.indexOf(QStringLiteral("Write-Output 'RepairRecommended: Yes'"));
    const qsizetype notfound_at =
        script.indexOf(QStringLiteral("Write-Output 'CorruptFile: NotFound'"));
    const qsizetype no_at = script.indexOf(QStringLiteral("Write-Output 'RepairRecommended: No'"));
    QVERIFY2(detected_at >= 0, "corruption branch must report 'CorruptFile: Detected'");
    QVERIFY2(yes_at > detected_at, "the corruption branch must be the one recommending a repair");
    QVERIFY2(notfound_at > yes_at, "the clean branch follows the corruption branch");
    QVERIFY2(no_at > notfound_at, "the clean branch must be the one recommending no repair");

    // The drive argument must route BOTH substitution sites -- the $drive variable (Test-Path
    // probe plus the "Drive:" line the report is keyed on) and the volume actually scanned -- so
    // a non-C volume is never scanned or labelled as C.
    const QString script_d = Action::buildScanVolumeScript(QChar('D'));
    QVERIFY(script_d.contains(QStringLiteral("$drive = \"D:\"")));
    QVERIFY(script_d.contains(QStringLiteral("Write-Output \"Drive: $drive\"")));
    QVERIFY(script_d.contains(QStringLiteral("Repair-Volume -DriveLetter D -Scan")));
    QVERIFY2(!script_d.contains(QStringLiteral("-DriveLetter C")),
             "scan must target the requested volume, not a hardcoded C");
    QVERIFY2(!script_d.contains(QStringLiteral("$drive = \"C:\"")),
             "the reported drive must be the requested volume, not a hardcoded C");
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

    // Over-count clamp: more blocks parsed than drives enumerated (one drive's stdout carrying
    // two ===SCAN_START===/===SCAN_END=== pairs) must never report a NEGATIVE shortfall, which
    // would flip a clean run to failure. Every case above has scanned <= total, so the clamp is
    // a guard the fixture never reached.
    auto over_counted = Action::evaluateDiskCheckOutcome(3, 2);
    QCOMPARE(over_counted.drives_failed, 0);
    QVERIFY2(over_counted.success,
             "every enumerated drive was scanned; an over-count must not fail the run");
}

QTEST_GUILESS_MAIN(CheckDiskErrorsActionTests)
#include "test_check_disk_errors_action.moc"
