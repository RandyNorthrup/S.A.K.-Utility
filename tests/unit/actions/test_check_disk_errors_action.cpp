// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_check_disk_errors_action.cpp
/// @brief Unit tests for CheckDiskErrorsAction pure seams (Codex B6-09/10):
///        the check must never schedule an offline (mutating) repair, and the
///        overall outcome must reflect every drive, not just one that succeeded.

#include "sak/actions/check_disk_errors_action.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_script_builder.h"
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
    void aDriveWithNoResultSaysWhyInReadableWords();
    void executeSaysItSkippedANetworkDrive();
    void executeStopsItsScanWhenCancelled();
    void aStopDuringTheScanSaysWindowsMayFinishIt();
    void aScanProcessThatWasCancelledEndsTheRun();
    void reportSaysNetworkDrivesAreNotChecked();
    void aRemovableNtfsDriveGetsCheckFileSystemsReadOnlyCheck();
    void aRemovableNtfsDriveNotCheckedSaysWhy();
    void aFixedDriveKeepsTheOnlineScan();
    void bothChecksAreBuiltFromTheSameStatements();

private:
    /// What parseDriveScanResult counted for one drive.
    struct ScanTotals {
        int scanned = 0;
        int errors = 0;
        int repairs = 0;
    };
    /// The report one drive's scan @p output makes, and what it counted.
    static QString reportOf(const QString& output, ScanTotals* totals) {
        Action action;
        QString report;
        action.parseDriveScanResult(
            output, report, totals->scanned, totals->errors, totals->repairs);
        return report;
    }
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
    /// What the scan had written when the cancel stopped it.
    QString output_when_cancelled;

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
            proc.std_out = output_when_cancelled;
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
    // Counted where it is named: the summary said "0 could not be checked" under this drive.
    QVERIFY2(result.log.contains(
                 QStringLiteral("1 of 2 drives scanned.\nCould not be checked: 1.\n")),
             qPrintable(result.log));
    QVERIFY2(result.message.contains(QStringLiteral("(U: is FAT32)")), qPrintable(result.message));
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

// The reason for a drive with no result is PowerShell's message alone, from a Storage cmdlet's
// record captured on testpc1: not its name, its position or its CategoryInfo lines.
void CheckDiskErrorsActionTests::aDriveWithNoResultSaysWhyInReadableWords() {
    class RecordScan final : public ScriptedCheckDisk {
    protected:
        sak::ProcessResult runScanProcess(const QString& script,
                                          int,
                                          const std::function<bool()>&) override {
            scripts_run << script;
            sak::ProcessResult proc;
            proc.exit_code = 0;
            proc.std_err = QStringLiteral(
                "Get-Partition : No MSFT_Partition objects found with property 'DiskNumber' equal "
                "to "
                "'99'.  Verify the value of the \r\n"
                "property and retry.\r\n"
                "At line:1 char:2\r\n"
                "+ (Get-Partition -DiskNumber 99 | Get-Volume | Where-Object {$_.DriveLe ...\r\n"
                "+  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~\r\n"
                "    + CategoryInfo          : ObjectNotFound: (99:UInt32) [Get-Partition], "
                "CimJobException\r\n"
                "    + FullyQualifiedErrorId : "
                "CmdletizationQuery_NotFound_DiskNumber,Get-Partition\r\n");
            return proc;
        }
    };
    RecordScan action;
    action.volumes = {volume("D:/", kFixed)};
    action.execute();
    const QString log = action.lastExecutionResult().log;
    QVERIFY2(log.contains(QStringLiteral("Drive D: - the scan gave no result (") +
                          QStringLiteral("No MSFT_Partition objects found with property "
                                         "'DiskNumber' equal to '99'.  Verify the "
                                         "value of the property and retry.") +
                          QLatin1Char(')')),
             qPrintable(log));
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
    QVERIFY2(result.log.contains(QStringLiteral("1 of 1 drive scanned")), qPrintable(result.log));
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
    // Review 5 (Z23): the summary was one line, ", " between its parts, that ran past the log
    // window, so what showed read "0 could not be checked, Network drives are not checked.". Each
    // line of the summary is a sentence of its own.
    const QString log = action.lastExecutionResult().log;
    const QStringList lines =
        log.mid(log.lastIndexOf(QStringLiteral("Summary: "))).split(QLatin1Char('\n'));
    QVERIFY(lines.size() >= 4);
    for (const QString& line : lines.mid(0, 4)) {
        QVERIFY2(line.endsWith(QLatin1Char('.')), qPrintable(line));
    }
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
    QCOMPARE(action.lastExecutionResult().message, QStringLiteral("Disk error check cancelled"));
}

// Repair-Volume -Scan is a storage job Windows finishes after its script is killed, so a stop once
// the scan has started is not reported as cancelled.
void CheckDiskErrorsActionTests::aStopDuringTheScanSaysWindowsMayFinishIt() {
    ScriptedCheckDisk action;
    action.volumes = {volume("C:/", kFixed), volume("F:/", kFixed)};
    action.cancel_during_scan = true;
    action.output_when_cancelled = QStringLiteral(
        "===SCAN_START===\r\nDrive: C:\r\nRunning online scan...\r\n"
        "SAK-WINDOWS-CONTINUES|BEGIN|volume-scan|0\r\n");

    action.execute();

    QCOMPARE(action.scripts_run.size(), 1);
    QCOMPARE(action.status(), sak::QuickAction::ActionStatus::Cancelled);
    QCOMPARE(action.lastExecutionResult().message,
             QStringLiteral("Disk error check stopped. Windows may still finish the read-only scan "
                            "of C: it had started; that scan changes nothing on the drive."));
    QCOMPARE(Action::cancelledMessage(QStringLiteral("C:")), action.lastExecutionResult().message);
    QCOMPARE(Action::cancelledMessage({}), QStringLiteral("Disk error check cancelled"));
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
    // Review 4 (Y14): Windows' result name stood above the same verdict in words.
    QVERIFY2(!report.contains(QStringLiteral("ScanNoErrorsFound")), qPrintable(report));
    QVERIFY2(report.contains(QStringLiteral("Drive C: ") + QChar(0x2713) +
                             QStringLiteral(" No errors found")),
             qPrintable(report));

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
                 "try { try { $scanResult = Repair-Volume -DriveLetter $sakScanDrive -Scan "
                 "-ErrorAction Stop } catch {")),
             "the one Repair-Volume call must be exactly the read-only -Scan form");
    QVERIFY(script.contains(QStringLiteral("$sakScanDrive = 'C';")));
    // The removable drive's check is chkdsk without /f, /r, /x or /scan: it only reads.
    for (const QString& flag : {QStringLiteral(" /f"),
                                QStringLiteral(" /r"),
                                QStringLiteral(" /x"),
                                QStringLiteral("/scan")}) {
        QVERIFY2(!script.contains(flag, Qt::CaseInsensitive), qPrintable(flag));
    }
    // Windows finishes a scan it has started even when this script is killed, so the scan runs
    // between the markers that say so.
    QVERIFY(script.contains(QStringLiteral("SAK-WINDOWS-CONTINUES|BEGIN|volume-scan|")));
    QVERIFY(script.contains(QStringLiteral("SAK-WINDOWS-CONTINUES|END|volume-scan")));
    // A drive removed right after an online scan can crash Windows (volsnap, 2026-11-12), so the
    // settle runs in a finally around the scan, passed or failed, before the result is reported.
    const QString scan_end = QStringLiteral(
        "[Console]::Out.WriteLine('SAK-WINDOWS-CONTINUES|END|volume-scan'); [Console]::Out.Flush() "
        "} } finally { ");
    const QString settle = QStringLiteral(
        "Write-Output 'Letting Windows finish with the volume before reporting the result (a "
        "drive removed right after a scan can crash Windows).'; Start-Sleep -Seconds 15 }\n");
    QVERIFY2(script.contains(scan_end + settle), qPrintable(script));
    QVERIFY2(script.contains(QStringLiteral("    try { [Console]::Out.WriteLine(('SAK-WINDOWS-"
                                            "CONTINUES|BEGIN|volume-scan|")),
             qPrintable(script));
    QVERIFY(script.indexOf(settle) < script.indexOf(QStringLiteral("ScanResult: $scanResult")));

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
    QVERIFY(script_d.contains(QStringLiteral("$sakScanDrive = 'D';")));
    QVERIFY2(!script_d.contains(QStringLiteral("-DriveLetter C")),
             "scan must target the requested volume, not a hardcoded C");
    QVERIFY2(!script_d.contains(QStringLiteral("$drive = \"C:\"")),
             "the reported drive must be the requested volume, not a hardcoded C");
}

namespace {

/// What one drive's scan finds, in the stand-ins its script runs against: the volume's file
/// system and drive type, the lock (empty: it locks; otherwise what it throws), chkdsk's exit
/// code and verdict line, and Repair-Volume's error (with its FullyQualifiedErrorId) or nothing.
struct StandIn {
    QString drive_type;
    QString lock_error;
    int chkdsk_exit{0};
    QString chkdsk_says;
    QString repair_error;
    QString repair_error_id;
};

/// Runs Check Disk Errors' script for D: in Windows PowerShell 5.1 against stand-ins for
/// Get-Volume, the lock, chkdsk, Repair-Volume and Start-Sleep: nothing reaches a real drive.
QString runAgainstStandIns(const StandIn& in) {
    QString script = CheckDiskErrorsAction::buildScanVolumeScript(QLatin1Char('D'));
    const QString chkdsk = QStringLiteral("([Environment]::SystemDirectory + '\\chkdsk.exe')");
    const QString lock = QStringLiteral("[Sak.ScanVolumeLock]::LockAndDismount($sakScanDrive)");
    QTemporaryDir dir;
    if (!dir.isValid() || !script.contains(chkdsk) || !script.contains(lock)) {
        return QStringLiteral("NO STAND-IN FOR CHKDSK");
    }
    script.replace(lock, QStringLiteral("(Invoke-StandInLock $sakScanDrive)"));
    const QString tool = QDir::toNativeSeparators(dir.filePath(QStringLiteral("chkdsk.cmd")));
    QFile file(tool);
    const QByteArray bytes =
        QStringLiteral(
            "@echo off\r\necho CHKDSK RAN\r\n%1echo stand-in chkdsk stderr line 1>&2\r\n"
            "exit /b %2\r\n")
            .arg(in.chkdsk_says.isEmpty() ? QString()
                                          : QStringLiteral("echo %1\r\n").arg(in.chkdsk_says))
            .arg(in.chkdsk_exit)
            .toLatin1();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return QStringLiteral("NO STAND-IN FOR CHKDSK");
    }
    file.close();
    using sak::PartitionScriptBuilder;
    script.replace(chkdsk, PartitionScriptBuilder::quotePowerShell(tool));
    const QString prelude =
        QStringLiteral(
            "function Get-Volume { param($DriveLetter, $ErrorAction) [pscustomobject]@{ "
            "FileSystem = 'NTFS'; DriveType = %1 } }\n"
            "function Invoke-StandInLock { param($Letter) Write-Host ('LOCKED ' + $Letter); "
            "if (%2) { throw %2 } }\n"
            "function Repair-Volume { [CmdletBinding()] param($DriveLetter, [switch]$Scan) "
            "Write-Host ('CALL Repair ' + $DriveLetter); if (%3) { "
            "$PSCmdlet.ThrowTerminatingError((New-Object "
            "System.Management.Automation.ErrorRecord ([Exception]%3), %4, 'NotSpecified', "
            "$null)) }; 'NoErrorsFound' }\n"
            "function Start-Sleep { param($Seconds) Write-Output ('SLEPT ' + $Seconds) }\n")
            .arg(PartitionScriptBuilder::quotePowerShell(in.drive_type),
                 PartitionScriptBuilder::quotePowerShell(in.lock_error),
                 PartitionScriptBuilder::quotePowerShell(in.repair_error),
                 PartitionScriptBuilder::quotePowerShell(in.repair_error_id));
    // Run from a file, so the command line names no real tool: the test hardware guard refuses a
    // PowerShell whose command line does.
    const QString runner = QDir::toNativeSeparators(dir.filePath(QStringLiteral("scan.ps1")));
    QFile runner_file(runner);
    const QByteArray runner_bytes = (prelude + script).toUtf8();
    if (!runner_file.open(QIODevice::WriteOnly) ||
        runner_file.write(runner_bytes) != runner_bytes.size()) {
        return QStringLiteral("NO STAND-IN RUNNER");
    }
    runner_file.close();
    constexpr int kStandInTimeoutMs = 60'000;
    const sak::ProcessResult proc =
        sak::runPowerShell(QStringLiteral("& %1; exit $LASTEXITCODE")
                               .arg(PartitionScriptBuilder::quotePowerShell(runner)),
                           kStandInTimeoutMs);
    return proc.std_out + proc.std_err;
}

}  // namespace

// Windows does not snapshot a removable drive, so the online scan of an NTFS USB stick failed "A
// snapshot error occurred while scanning this drive. Run an offline scan and fix.", here as in
// Check File System (the coordinator, 2026-11-13). Such a drive now gets Check File System's own
// read-only check: locked (one in use is said to be, and not checked), dismounted, and read by
// chkdsk without /f, and its result is one of three: no errors found, errors found (none fixed),
// or not checked. A fixed drive whose snapshot fails is said in the same words as there, not as
// Windows' advice to run a scan that fixes. Windows PowerShell 5.1 runs the whole script.
void CheckDiskErrorsActionTests::aRemovableNtfsDriveGetsCheckFileSystemsReadOnlyCheck() {
    const QString settle = QStringLiteral("Letting Windows finish with the volume");
    const QString clean = runAgainstStandIns({.drive_type = QStringLiteral("Removable")});
    QVERIFY2(clean.indexOf(QStringLiteral("LOCKED D")) >= 0, qPrintable(clean));
    QVERIFY2(clean.indexOf(QStringLiteral("LOCKED D")) <
                 clean.indexOf(QStringLiteral("CHKDSK RAN")),
             qPrintable(clean));
    QVERIFY2(clean.contains(settle) && clean.contains(QStringLiteral("SLEPT 15")),
             qPrintable(clean));
    QVERIFY2(!clean.contains(QStringLiteral("CALL Repair")), qPrintable(clean));
    ScanTotals totals;
    QString report = reportOf(clean, &totals);
    QCOMPARE(totals.scanned, 1);
    QCOMPARE(totals.errors, 0);
    QVERIFY2(report.contains(QStringLiteral("Drive D: ") + QChar(0x2713) +
                             QStringLiteral(" No errors found (read-only chkdsk)")),
             qPrintable(report + clean));

    // What chkdsk said, reading only, on testpc1's stick with one file record damaged.
    const QString damaged =
        runAgainstStandIns({.drive_type = QStringLiteral("Removable"),
                            .chkdsk_exit = 3,
                            .chkdsk_says = QStringLiteral(
                                "Errors found.  CHKDSK cannot continue in read-only mode.")});
    totals = {};
    report = reportOf(damaged, &totals);
    QCOMPARE(totals.scanned, 1);
    QCOMPARE(totals.errors, 1);
    QCOMPARE(totals.repairs, 1);
    QVERIFY2(report.contains(QStringLiteral(
                 "Drive D: \u26a0 Errors found by a read-only chkdsk, which fixed none of them")),
             qPrintable(report + damaged));
    QVERIFY2(report.contains(QStringLiteral("Repair recommended")), qPrintable(report));
}

// A removable drive the read-only check could not read is named as not checked, with why: in use
// (and chkdsk never run), or chkdsk's own last line.
void CheckDiskErrorsActionTests::aRemovableNtfsDriveNotCheckedSaysWhy() {
    const QString in_use = QStringLiteral(
        "D: is in use, so it was not checked. Close the files and programs using it, then check "
        "it again. Nothing was changed.");
    const QString held =
        runAgainstStandIns({.drive_type = QStringLiteral("Removable"), .lock_error = in_use});
    ScanTotals totals;
    QString report = reportOf(held, &totals);
    QCOMPARE(totals.scanned, 0);
    QVERIFY2(report.contains(QStringLiteral("Drive D: - Not checked (") + in_use +
                             QLatin1Char(')')),
             qPrintable(report + held));
    QVERIFY2(!held.contains(QStringLiteral("CHKDSK RAN")), qPrintable(held));
    QVERIFY2(held.contains(QStringLiteral("Letting Windows finish with the volume")),
             qPrintable(held));

    const QString unread =
        runAgainstStandIns({.drive_type = QStringLiteral("Removable"), .chkdsk_exit = 3});
    totals = {};
    report = reportOf(unread, &totals);
    QCOMPARE(totals.scanned, 0);
    QVERIFY2(report.contains(QStringLiteral(
                 "Drive D: - Not checked (chkdsk could not check D: (exit 3): stand-in chkdsk "
                 "stderr line)")),
             qPrintable(report + unread));
}

// A fixed drive keeps the online scan, and a snapshot it cannot take is said in Check File System's
// words, without Windows' advice to run a scan that fixes; both settle after the scan.
void CheckDiskErrorsActionTests::aFixedDriveKeepsTheOnlineScan() {
    const QString settle = QStringLiteral("Letting Windows finish with the volume");
    const QString fixed = runAgainstStandIns({.drive_type = QStringLiteral("Fixed")});
    ScanTotals totals;
    QString report = reportOf(fixed, &totals);
    QCOMPARE(totals.scanned, 1);
    QVERIFY2(fixed.contains(QStringLiteral("CALL Repair D")), qPrintable(fixed));
    QVERIFY2(!fixed.contains(QStringLiteral("LOCKED")), qPrintable(fixed));
    QVERIFY2(!fixed.contains(QStringLiteral("CHKDSK RAN")), qPrintable(fixed));
    QVERIFY2(fixed.contains(settle), qPrintable(fixed));
    QVERIFY2(report.contains(QStringLiteral("Drive D: ") + QChar(0x2713) +
                             QStringLiteral(" No errors found\n")),
             qPrintable(report + fixed));

    const QString snapshot = runAgainstStandIns(
        {.drive_type = QStringLiteral("Fixed"),
         .repair_error = QStringLiteral(
             "A snapshot error occurred while scanning this drive. Run an offline scan and fix."),
         .repair_error_id = QStringLiteral("StorageWMI 43012,Repair-Volume")});
    totals = {};
    report = reportOf(snapshot, &totals);
    QCOMPARE(totals.scanned, 0);
    QVERIFY2(report.contains(QStringLiteral(
                 "Drive D: - Scan failed (Windows could not take the snapshot it reads D: through, "
                 "so D: was not checked. Nothing was changed.)")),
             qPrintable(report + snapshot));
    QVERIFY2(!report.contains(QStringLiteral("offline scan")), qPrintable(report));
    QVERIFY2(snapshot.contains(settle), qPrintable(snapshot));
}

// One check, two callers: Check Disk Errors' script is built from the statements Check File
// System's is, the target, the online scan and the read-only chkdsk, not a copy of them.
void CheckDiskErrorsActionTests::bothChecksAreBuiltFromTheSameStatements() {
    using sak::PartitionScriptBuilder;
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Partition;
    target.disk_number = sak::DiskNumber{1};
    target.partition_number = sak::PartitionNumber{2};
    target.drive_letter = QStringLiteral("D");
    target.size_bytes = 256ULL * 1024ULL * 1024ULL;
    const auto check_file_system = PartitionScriptBuilder().buildScript(
        sak::PartitionOperationPlanner::makeOperation(sak::PartitionOperationType::CheckFileSystem,
                                                      target));
    QVERIFY2(check_file_system.valid(), qPrintable(check_file_system.blockers.join(' ')));
    const QString check_disk = Action::buildScanVolumeScript(QLatin1Char('D'));
    const QString read_only = PartitionScriptBuilder::readOnlyVolumeCheckStatement();
    const QString target_statement =
        PartitionScriptBuilder::volumeScanTargetStatement(QStringLiteral("'D'"));
    QVERIFY(read_only.contains(QStringLiteral("LockAndDismount")));
    QVERIFY2(check_disk.contains(read_only) && check_file_system.script.contains(read_only),
             qPrintable(check_disk));
    QVERIFY2(check_disk.contains(target_statement) &&
                 check_file_system.script.contains(target_statement),
             qPrintable(check_file_system.script));
    QVERIFY(check_disk.contains(
        PartitionScriptBuilder::onlineVolumeScanStatement(QStringLiteral("$scanResult"), false)));
    QVERIFY(check_file_system.script.contains(
        PartitionScriptBuilder::onlineVolumeScanStatement(QString(), true)));
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
