// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_check_disk_errors_action.cpp
/// @brief Unit tests for CheckDiskErrorsAction pure seams (Codex B6-09/10):
///        the check must never schedule an offline (mutating) repair, and the
///        overall outcome must reflect every drive, not just one that succeeded.

#include "sak/actions/check_disk_errors_action.h"
#include "sak/process_runner.h"

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
    void selectScanTargets_setsNetworkDrivesApart();
    void aFat32StickIsNamedAsNotCheckedNotFailed();
    void aDriveWithNoResultIsNamed();
    void executeSaysItSkippedANetworkDrive();
    void executeStopsItsScanWhenCancelled();
    void aScanProcessThatWasCancelledEndsTheRun();
    void reportSaysNetworkDrivesAreNotChecked();
};

namespace {
constexpr unsigned kFixed = 3;   // DRIVE_FIXED
constexpr unsigned kRemote = 4;  // DRIVE_REMOTE

CheckDiskErrorsAction::MountedVolume volume(const char* root,
                                            unsigned type,
                                            bool read_only = false) {
    return {.root = QString::fromLatin1(root),
            .valid = true,
            .read_only = read_only,
            .drive_type = type};
}

/// Runs the action's real flow over a volume list and scan output of the test's choosing. Nothing
/// reaches a real drive: no Repair-Volume, not even PowerShell.
class ScriptedCheckDisk : public CheckDiskErrorsAction {
public:
    QVector<MountedVolume> volumes;
    QStringList scripts_run;
    bool cancel_during_scan{false};
    bool report_process_cancelled{false};
    bool saw_cancel_through_callback{false};

protected:
    QVector<MountedVolume> mountedVolumes() const override { return volumes; }

    using ProcessResult = sak::ProcessResult;

    ProcessResult runScanProcess(const QString& script,
                                 int,
                                 const std::function<bool()>& should_cancel) override {
        scripts_run << script;
        ProcessResult proc;
        proc.exit_code = 0;
        if (report_process_cancelled) {
            proc.cancelled = true;
            return proc;
        }
        if (cancel_during_scan) {
            cancel();
            saw_cancel_through_callback = should_cancel();
            proc.cancelled = saw_cancel_through_callback;
            return proc;
        }
        proc.std_out = QStringLiteral(
            "===SCAN_START===\nDrive: C:\nScanResult: NoErrorsFound\n===SCAN_END===\n");
        return proc;
    }
};
}  // namespace

void CheckDiskErrorsActionTests::selectScanTargets_setsNetworkDrivesApart() {
    const auto targets = Action::selectScanTargets({volume("C:/", kFixed),
                                                    volume("Z:/", kRemote),
                                                    volume("Y:/", kRemote, true),
                                                    volume("Z:/", kRemote),
                                                    volume("C:/Mount/Foreign/", kFixed),
                                                    volume("E:/", kFixed, true),
                                                    volume("F:/", kFixed)});
    QCOMPARE(targets.drives, (QVector<QChar>{QLatin1Char('C'), QLatin1Char('F')}));
    QCOMPARE(targets.network_drives, (QVector<QChar>{QLatin1Char('Z'), QLatin1Char('Y')}));
}

// U:, a FAT32 stick, failed "The specified file system is not supported" and took the whole run to
// "Execution failed". The online scan reads NTFS and ReFS only, so it is set apart with the reason.
void CheckDiskErrorsActionTests::aFat32StickIsNamedAsNotCheckedNotFailed() {
    ScriptedCheckDisk action;
    auto stick = volume("U:/", kFixed);
    stick.file_system = QStringLiteral("FAT32");
    auto system = volume("C:/", kFixed);
    system.file_system = QStringLiteral("NTFS");
    action.volumes = {system, stick};

    action.execute();

    const auto& result = action.lastExecutionResult();
    QCOMPARE(action.scripts_run.size(), 1);
    QVERIFY2(result.success, qPrintable(result.message));
    QVERIFY2(result.log.contains(QStringLiteral("Drive U: - not checked: it is FAT32")),
             qPrintable(result.log));
    QVERIFY2(result.log.contains(QStringLiteral("chkdsk U:")), qPrintable(result.log));
    QVERIFY(Action::fileSystemIsScannable(QStringLiteral("NTFS")));
    QVERIFY(Action::fileSystemIsScannable(QStringLiteral("ReFS")));
    QVERIFY(Action::fileSystemIsScannable(QString()));
    QVERIFY(!Action::fileSystemIsScannable(QStringLiteral("exFAT")));
}

// One run said "Scanned 4 of 5 drive(s); 1 could not be checked" and its Details named no drive.
void CheckDiskErrorsActionTests::aDriveWithNoResultIsNamed() {
    class SilentScan final : public ScriptedCheckDisk {
    protected:
        sak::ProcessResult runScanProcess(const QString& script,
                                          int,
                                          const std::function<bool()>&) override {
            scripts_run << script;
            sak::ProcessResult proc;
            proc.exit_code = 0;
            proc.std_err = QStringLiteral("Repair-Volume printed nothing");
            return proc;
        }
    };
    SilentScan action;
    action.volumes = {volume("D:/", kFixed)};
    action.execute();
    const auto& result = action.lastExecutionResult();
    QVERIFY(!result.success);
    QVERIFY2(result.log.contains(QStringLiteral(
                 "Drive D: - the scan gave no result (Repair-Volume printed nothing)")),
             qPrintable(result.log));
    QVERIFY2(result.message.contains(QStringLiteral("(D:)")) ||
                 result.message.contains(QStringLiteral("Could not scan any drives")),
             qPrintable(result.message));
}

void CheckDiskErrorsActionTests::executeSaysItSkippedANetworkDrive() {
    ScriptedCheckDisk action;
    action.volumes = {volume("C:/", kFixed), volume("Z:/", kRemote)};

    action.execute();

    const auto& result = action.lastExecutionResult();
    QCOMPARE(action.scripts_run.size(), 1);
    QVERIFY(!action.scripts_run.first().contains(QStringLiteral("Z:")));
    QVERIFY2(result.log.contains(QStringLiteral("Drive Z: - skipped, it is a network drive")),
             qPrintable(result.log));
    // Left out on purpose, so it is not counted as a drive that could not be checked.
    QVERIFY2(result.success, qPrintable(result.message));
    QVERIFY2(result.log.contains(QStringLiteral("1 of 1 drive(s) scanned")),
             qPrintable(result.log));
}

void CheckDiskErrorsActionTests::reportSaysNetworkDrivesAreNotChecked() {
    ScriptedCheckDisk action;
    action.volumes = {volume("C:/", kFixed)};

    action.execute();

    // Elevated, Windows may hide the signed-in user's mapped drives altogether, so the report
    // says this even with none to name.
    QVERIFY2(action.lastExecutionResult().log.contains(
                 QStringLiteral("Network drives are not checked.")),
             qPrintable(action.lastExecutionResult().log));
}

// The process's own word that it was stopped is enough, whatever the action's flag says by then.
void CheckDiskErrorsActionTests::aScanProcessThatWasCancelledEndsTheRun() {
    ScriptedCheckDisk action;
    action.volumes = {volume("C:/", kFixed), volume("F:/", kFixed)};
    action.report_process_cancelled = true;

    action.execute();

    QCOMPARE(action.scripts_run.size(), 1);  // F: never starts
}

void CheckDiskErrorsActionTests::executeStopsItsScanWhenCancelled() {
    ScriptedCheckDisk action;
    action.volumes = {volume("C:/", kFixed), volume("F:/", kFixed)};
    action.cancel_during_scan = true;

    action.execute();

    QVERIFY2(action.saw_cancel_through_callback, "the scan is handed a callback that sees cancel");
    QCOMPARE(action.scripts_run.size(), 1);  // F: never starts
    QCOMPARE(action.status(), sak::QuickAction::ActionStatus::Cancelled);
    QVERIFY(!action.lastExecutionResult().success);
}

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
