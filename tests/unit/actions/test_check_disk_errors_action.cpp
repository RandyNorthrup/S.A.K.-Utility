// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_check_disk_errors_action.cpp
/// @brief Unit tests for CheckDiskErrorsAction pure seams (Codex B6-09/10):
///        the check must never schedule an offline (mutating) repair, and the
///        overall outcome must reflect every drive, not just one that succeeded.

#include "sak/actions/check_disk_errors_action.h"

#include <QtTest/QtTest>

#include <windows.h>

using sak::CheckDiskErrorsAction;

class CheckDiskErrorsActionTests : public QObject {
    Q_OBJECT

    using Action = CheckDiskErrorsAction;

private Q_SLOTS:
    // B6-09: a "check" must not mutate the disk.
    void buildScanVolumeScript_isReadOnly();
    // B6-10: failed/timed-out drives must affect the outcome.
    void evaluateDiskCheckOutcome_aggregatesAllDrives();
    // The scan's own result decides, and only a clean result reads clean.
    void classifyScanResult_coversEveryRepairStatus();
    void parseDriveScanResult_reportsWhatTheScanReturned();
    void driveTypeIsScannable_onlyLocalVolumes();
    void runIsBoundByItsOwnTimeLimit();
};

// Every name is from the Volume.RepairStatus enumeration in Windows' own Storage module.
void CheckDiskErrorsActionTests::classifyScanResult_coversEveryRepairStatus() {
    using Verdict = Action::ScanVerdict;
    for (const char* name : {"NoErrorsFound", "ScanNoErrorsFound"}) {
        QCOMPARE(Action::classifyScanResult(QString::fromLatin1(name)), Verdict::NoErrors);
    }
    for (const char* name : {"ErrorsFixed",
                             "MinorErrorsFixedOrCleanup",
                             "ScanErrorsFoundAndFixedOnline",
                             "ScanFoundAndFixedLeaks"}) {
        QCOMPARE(Action::classifyScanResult(QString::fromLatin1(name)), Verdict::FixedOnline);
    }
    for (const char* name : {"ScanErrorsFixedOnlineAlsoNeedSpotFix",
                             "ScanErrorsFoundNeedSpotFix",
                             "NeedOfflineScanAndFix",
                             "ScanFoundLeaks"}) {
        QCOMPARE(Action::classifyScanResult(QString::fromLatin1(name)), Verdict::NeedsRepair);
    }
    for (const char* name : {"Failed",
                             "ScanNeedsRetry",
                             "ScanRunning",
                             "ScanTransientSnapshotError",
                             "ScanFatalSnapshotError",
                             "ScanNeedsMetaDataRetry",
                             "TriageComplete",
                             "TriageUncertain",
                             "TriageFailed",
                             "SalvageDone",
                             "SomethingAFutureWindowsAdds",
                             ""}) {
        QCOMPARE(Action::classifyScanResult(QString::fromLatin1(name)), Verdict::Inconclusive);
    }
}

void CheckDiskErrorsActionTests::parseDriveScanResult_reportsWhatTheScanReturned() {
    const auto outputFor = [](const QString& result) {
        return QStringLiteral(
                   "===SCAN_START===\nDrive: C:\nRunning online scan...\n"
                   "ScanResult: %1\n===SCAN_END===\n")
            .arg(result);
    };
    struct Totals {
        int scanned = 0;
        int errors = 0;
        int repairs = 0;
    };
    Action action;
    const auto parse = [&action](const QString& output, QString* report) {
        Totals totals;
        action.parseDriveScanResult(output, *report, totals.scanned, totals.errors, totals.repairs);
        return totals;
    };

    QString report;
    Totals totals = parse(outputFor(QStringLiteral("ScanNoErrorsFound")), &report);
    QCOMPARE(totals.scanned, 1);
    QCOMPARE(totals.errors, 0);
    QCOMPARE(totals.repairs, 0);
    QVERIFY2(report.contains(QStringLiteral("No errors found")), qPrintable(report));

    // The case this exists for: errors reported by the scan, no marker file anywhere. The old
    // check read this as clean.
    report.clear();
    totals = parse(outputFor(QStringLiteral("ScanErrorsFoundNeedSpotFix")), &report);
    QCOMPARE(totals.scanned, 1);
    QCOMPARE(totals.errors, 1);
    QCOMPARE(totals.repairs, 1);
    QVERIFY2(report.contains(QStringLiteral("Repair recommended")), qPrintable(report));

    report.clear();
    totals = parse(outputFor(QStringLiteral("ScanErrorsFoundAndFixedOnline")), &report);
    QCOMPARE(totals.scanned, 1);
    QCOMPARE(totals.errors, 1);
    QCOMPARE(totals.repairs, 0);

    // An answer that says nothing about the volume is not a scanned drive.
    report.clear();
    totals = parse(outputFor(QStringLiteral("ScanNeedsRetry")), &report);
    QCOMPARE(totals.scanned, 0);
    QCOMPARE(totals.errors, 0);
    QVERIFY2(report.contains(QStringLiteral("ScanNeedsRetry")), qPrintable(report));

    // Cut short after the scan line opened: same.
    report.clear();
    totals = parse(QStringLiteral("===SCAN_START===\nDrive: C:\n===SCAN_END===\n"), &report);
    QCOMPARE(totals.scanned, 0);

    // A failed scan keeps the real reason, colons and all.
    report.clear();
    totals = parse(QStringLiteral("===SCAN_START===\nDrive: C:\nError: Access denied: code 5\n"
                                  "Status: Scan failed\n===SCAN_END===\n"),
                   &report);
    QCOMPARE(totals.scanned, 0);
    QVERIFY2(report.contains(QStringLiteral("Access denied: code 5")), qPrintable(report));
}

void CheckDiskErrorsActionTests::driveTypeIsScannable_onlyLocalVolumes() {
    QVERIFY(Action::driveTypeIsScannable(DRIVE_FIXED));
    QVERIFY(Action::driveTypeIsScannable(DRIVE_REMOVABLE));
    QVERIFY(!Action::driveTypeIsScannable(DRIVE_REMOTE));
    QVERIFY(!Action::driveTypeIsScannable(DRIVE_CDROM));
    QVERIFY(!Action::driveTypeIsScannable(DRIVE_RAMDISK));
    QVERIFY(!Action::driveTypeIsScannable(DRIVE_UNKNOWN));
    QVERIFY(!Action::driveTypeIsScannable(DRIVE_NO_ROOT_DIR));
}

// Thirty seconds cut off the scan of any large drive and called it failed.
void CheckDiskErrorsActionTests::runIsBoundByItsOwnTimeLimit() {
    Action action;
    QCOMPARE(action.executionTimeoutMs(), sak::kCheckDiskErrorsTimeoutMs);
    QVERIFY(sak::kCheckDiskErrorsTimeoutMs > sak::kDefaultQuickActionTimeoutMs);
}

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
    QVERIFY2(script.contains(QStringLiteral(
                 "$scanResult = Repair-Volume -DriveLetter C -Scan -ErrorAction Stop\n")),
             "the one Repair-Volume call must be exactly the read-only -Scan form");

    // The tokens parseDriveScanResult and processScanKeyValue match on by exact equality; rename
    // any of them and the whole scan pipeline silently degrades with no test noticing.
    QVERIFY2(script.contains(QStringLiteral("Write-Output '===SCAN_START==='")),
             "parseDriveScanResult opens a block only on exact '===SCAN_START==='");
    QVERIFY2(script.contains(QStringLiteral("Write-Output '===SCAN_END==='")),
             "the drive is counted as scanned only on exact '===SCAN_END==='");
    QVERIFY2(script.contains(QStringLiteral("Write-Output \"Drive: $drive\"")),
             "the drive letter reaches the report only via the exact 'Drive' key");
    // The verdict is what the scan returned, not whether some marker file happens to exist.
    QVERIFY2(script.contains(QStringLiteral("Write-Output \"ScanResult: $scanResult\"")),
             "the scan's own result must reach the parser");
    QVERIFY2(!script.contains(QStringLiteral("$corrupt")), "a marker file is not a scan result");

    // The drive argument must route BOTH substitution sites -- the $drive variable (the "Drive:"
    // line the report is keyed on) and the volume actually scanned -- so a non-C volume is never
    // scanned or labelled as C.
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
