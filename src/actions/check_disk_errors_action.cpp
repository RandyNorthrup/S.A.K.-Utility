// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file check_disk_errors_action.cpp
/// @brief Implements read-only online disk error checks using Repair-Volume -Scan

#include "sak/actions/check_disk_errors_action.h"

#include "sak/action_constants.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"
#include "sak/windows_continued_work.h"

#include <QDeadlineTimer>
#include <QStorageInfo>

#include <algorithm>

#include <windows.h>

namespace sak {

namespace {
constexpr int kBareDriveRootLength = 3;      ///< "C:/" -- the only form Repair-Volume can address.
constexpr int kDriveRootSeparatorIndex = 2;  ///< index of the path separator in "C:/".
constexpr int kDiskReportWidth = 78;
constexpr int kMinimumScanKeyValueParts = 2;
constexpr int kDriveScanProgressStart = 10;
constexpr int kDriveScanProgressSpan = 80;
// An online scan walks the file system's metadata, so its time follows how many files the drive
// holds rather than its size, and a large one can take far longer than the 30 seconds this used to
// allow. One drive gets half an hour; the whole run is bound by kCheckDiskErrorsTimeoutMs.
constexpr int kDriveScanTimeoutMs = sak::kTimeoutSystemRepairMs;

// Only a volume mounted at a bare drive-letter root ("C:/") can be addressed by Repair-Volume
// -DriveLetter. A directory-mounted volume ("C:/Mount/Foreign/") would otherwise be misread as its
// mount path's first letter and the host drive scanned in its place.
bool isBareDriveRoot(const QString& root) {
    return root.length() == kBareDriveRootLength && root.at(0).isLetter() &&
           root.at(1) == QLatin1Char(':') &&
           (root.at(kDriveRootSeparatorIndex) == QLatin1Char('/') ||
            root.at(kDriveRootSeparatorIndex) == QLatin1Char('\\'));
}

/// @brief Describe WHY a scan subprocess did not finish cleanly, surfacing the
///        authoritative process status -- cancellation, a crash exit-status, or
///        the child's exit code -- so the per-drive report states the real cause
///        instead of a generic "did not complete cleanly". Truncated output is
///        annotated because a capped stdout cannot be trusted even at exit 0.
QString describeProcessFailure(const ProcessResult& proc) {
    QString reason;
    if (proc.cancelled) {
        reason = QStringLiteral("cancelled");
    } else if (proc.exit_status != 0) {
        reason = QStringLiteral("process crashed (exit code %1)").arg(proc.exit_code);
    } else {
        reason = QStringLiteral("exit code %1").arg(proc.exit_code);
    }
    if (proc.output_truncated) {
        reason += QStringLiteral(", output truncated");
    }
    return reason;
}

QString drivesCount(qint64 count) {
    return countOf(count, QStringLiteral("drive"), QStringLiteral("drives"));
}

QString errorsCount(qint64 count) {
    return countOf(count, QStringLiteral("error"), QStringLiteral("errors"));
}

QString repairsCount(qint64 count) {
    return countOf(count, QStringLiteral("repair"), QStringLiteral("repairs"));
}

}  // namespace

CheckDiskErrorsAction::CheckDiskErrorsAction(QObject* parent) : QuickAction(parent) {}

QVector<CheckDiskErrorsAction::MountedVolume> CheckDiskErrorsAction::mountedVolumes() const {
    QVector<MountedVolume> volumes;
    const auto mounted = QStorageInfo::mountedVolumes();
    volumes.reserve(mounted.size());
    for (const QStorageInfo& storage : mounted) {
        const QString root = storage.rootPath();
        const std::wstring wide_root = root.toStdWString();
        volumes.append({.root = root,
                        .valid = storage.isValid(),
                        .read_only = storage.isReadOnly(),
                        .drive_type = GetDriveTypeW(wide_root.c_str()),
                        .file_system = QString::fromUtf8(storage.fileSystemType())});
    }
    return volumes;
}

CheckDiskErrorsAction::ScanTargets CheckDiskErrorsAction::selectScanTargets(
    const QVector<MountedVolume>& volumes) {
    ScanTargets targets;
    for (const MountedVolume& volume : volumes) {
        if (!isBareDriveRoot(volume.root)) {
            continue;
        }
        const QChar drive = volume.root.at(0).toUpper();
        // Repair-Volume cannot scan a network drive, and that is worth saying, not skipping
        // quietly.
        if (volume.drive_type == DRIVE_REMOTE) {
            if (!targets.network_drives.contains(drive)) {
                targets.network_drives.append(drive);
            }
            continue;
        }
        if (!volume.valid || volume.read_only || !driveTypeIsScannable(volume.drive_type)) {
            continue;
        }
        if (!fileSystemIsScannable(volume.file_system)) {
            noteUnsupportedVolume(&targets, drive, volume.file_system);
            continue;
        }
        // De-duplicate so one drive is never scanned (or counted) twice.
        if (!targets.drives.contains(drive)) {
            targets.drives.append(drive);
        }
    }
    return targets;
}

ProcessResult CheckDiskErrorsAction::runScanProcess(const QString& script,
                                                    int timeout_ms,
                                                    const std::function<bool()>& should_cancel) {
    return runPowerShell(script, timeout_ms, true, true, should_cancel);
}

void CheckDiskErrorsAction::noteUnsupportedVolume(ScanTargets* targets,
                                                  QChar drive,
                                                  const QString& file_system) {
    const bool listed = std::ranges::any_of(
        targets->unsupported, [drive](const UnsupportedVolume& u) { return u.drive == drive; });
    if (!listed) {
        targets->unsupported.append({.drive = drive, .file_system = file_system});
    }
}

bool CheckDiskErrorsAction::fileSystemIsScannable(const QString& file_system) {
    const QString name = file_system.trimmed();
    return name.isEmpty() || name.compare(QStringLiteral("NTFS"), Qt::CaseInsensitive) == 0 ||
           name.compare(QStringLiteral("ReFS"), Qt::CaseInsensitive) == 0;
}

bool CheckDiskErrorsAction::driveTypeIsScannable(unsigned int drive_type) {
    return drive_type == DRIVE_FIXED || drive_type == DRIVE_REMOVABLE;
}

CheckDiskErrorsAction::ScanVerdict CheckDiskErrorsAction::classifyScanResult(
    const QString& result) {
    // The names are the Volume.RepairStatus enumeration in the Storage module's Volume.cdxml.
    // Everything not listed -- Failed, ScanNeedsRetry, ScanRunning, the snapshot errors, the
    // triage states, or a name a later Windows adds -- is not an answer about this volume.
    static const QStringList kClean = {QStringLiteral("NoErrorsFound"),
                                       QStringLiteral("ScanNoErrorsFound")};
    static const QStringList kFixed = {QStringLiteral("ErrorsFixed"),
                                       QStringLiteral("MinorErrorsFixedOrCleanup"),
                                       QStringLiteral("ScanErrorsFoundAndFixedOnline"),
                                       QStringLiteral("ScanFoundAndFixedLeaks")};
    static const QStringList kNeedsRepair = {QStringLiteral("ScanErrorsFixedOnlineAlsoNeedSpotFix"),
                                             QStringLiteral("ScanErrorsFoundNeedSpotFix"),
                                             QStringLiteral("NeedOfflineScanAndFix"),
                                             QStringLiteral("ScanFoundLeaks")};
    if (kClean.contains(result)) {
        return ScanVerdict::NoErrors;
    }
    if (kFixed.contains(result)) {
        return ScanVerdict::FixedOnline;
    }
    if (kNeedsRepair.contains(result)) {
        return ScanVerdict::NeedsRepair;
    }
    return ScanVerdict::Inconclusive;
}

void CheckDiskErrorsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Disk error check cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    m_unchecked_drives.clear();
    m_unsupported_drives.clear();
    m_scan_left_to_windows.clear();
    const QDateTime start_time = QDateTime::currentDateTime();
    QVector<QChar> drives;
    QString report;
    if (!executeEnumerateVolumes(start_time, drives, report)) {
        return;
    }

    DiskCheckTotals totals;
    totals.total_drives = static_cast<int>(drives.count());
    executeRunVolumeScan(
        drives, report, totals.drives_scanned, totals.errors_found, totals.repairs_recommended);
    if (isCancelled()) {
        emitCancelledResult(cancelledMessage(m_scan_left_to_windows), start_time);
        return;
    }

    executeBuildReport(start_time, report, totals);
}

void CheckDiskErrorsAction::noteScanLeftToWindows(QChar drive, const QString& output) {
    // Stopping the script does not stop a scan Windows has already started.
    if (openWindowsContinuedPhase(output)) {
        m_scan_left_to_windows = QString(drive) + QLatin1Char(':');
    }
}

QString CheckDiskErrorsAction::cancelledMessage(const QString& scan_left_to_windows) {
    if (scan_left_to_windows.isEmpty()) {
        return QStringLiteral("Disk error check cancelled");
    }
    // Not "cancelled": the scan Windows had started goes on without this script.
    return QStringLiteral(
               "Disk error check stopped. Windows may still finish the read-only scan of %1 it "
               "had started; that scan changes nothing on the drive.")
        .arg(scan_left_to_windows);
}

bool CheckDiskErrorsAction::executeEnumerateVolumes(const QDateTime& start_time,
                                                    QVector<QChar>& drives,
                                                    QString& report) {
    Q_EMIT executionProgress("Detecting disk drives...", progress::kStep5);

    const ScanTargets targets = selectScanTargets(mountedVolumes());
    drives = targets.drives;
    QString skipped;
    for (const QChar network_drive : targets.network_drives) {
        skipped += QString("Drive %1: - skipped, it is a network drive\n\n").arg(network_drive);
    }
    for (const UnsupportedVolume& volume : targets.unsupported) {
        skipped += QString(
                       "Drive %1: - not checked: it is %2, and the online scan reads NTFS and "
                       "ReFS only. Run chkdsk %1: (without /f) to check it read-only\n\n")
                       .arg(volume.drive)
                       .arg(volume.file_system);
        m_unsupported_drives.append(
            QStringLiteral("%1: is %2").arg(volume.drive).arg(volume.file_system));
    }

    if (drives.isEmpty()) {
        ExecutionResult result;
        result.success = false;
        result.message = "No valid drives found for scanning";
        result.log = skipped + "Unable to detect any readable, writable volumes";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        finishWithResult(result, ActionStatus::Failed);
        return false;
    }

    report += "+" + QString("=").repeated(kDiskReportWidth) + "+\n";
    report += "|" + QString(" DISK ERROR CHECK & REPAIR REPORT").leftJustified(kDiskReportWidth) +
              "|\n";
    report += "+" + QString("=").repeated(kDiskReportWidth) + "+\n\n";
    report += skipped;

    return true;
}

QString CheckDiskErrorsAction::buildScanVolumeScript(QChar drive) {
    // Read-only check ONLY: Repair-Volume -Scan is a non-destructive online scan.
    // This action is a "Check Disk Errors" scan and must NOT mutate the disk, so
    // it never calls -OfflineScanAndFix (which schedules a boot-time repair). The verdict is the
    // result the scan itself returns; a repair is only ever recommended, never run.
    // The scan is a Windows storage job ("Repair Volume") that finishes even when this script is
    // killed, so it runs between the markers that say so. The settle runs in a finally around it,
    // whether the scan passed or failed: a drive removed right after an online scan can crash
    // Windows (see kVolumeScanSettleSeconds).
    return (QStringLiteral("$drive = \"%1:\"\n"
                           "Write-Output '===SCAN_START==='\n"
                           "Write-Output \"Drive: $drive\"\n"
                           "\n"
                           "try {\n"
                           "    Write-Output 'Running online scan...'\n"
                           "    ") +
            volumeScanSettledStatement(QStringLiteral("0"),
                                       QStringLiteral("$scanResult = Repair-Volume -DriveLetter %1 "
                                                      "-Scan -ErrorAction Stop")) +
            QStringLiteral("\n"
                           "    Write-Output \"ScanResult: $scanResult\"\n"
                           "} catch {\n"
                           "    Write-Output \"Error: $($_.Exception.Message)\"\n"
                           "    Write-Output 'Status: Scan failed'\n"
                           "}\n"
                           "\n"
                           "Write-Output '===SCAN_END==='\n"))
        .arg(drive);
}

bool CheckDiskErrorsAction::parseDriveScanResult(const QString& output,
                                                 QString& report,
                                                 int& drives_scanned,
                                                 int& errors_found,
                                                 int& repairs_recommended) {
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    bool parsing = false;
    bool entered = false;
    ParsedDriveState state;

    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();

        if (trimmed == "===SCAN_START===") {
            parsing = true;
            state = ParsedDriveState{};
            continue;
        }

        if (trimmed == "===SCAN_END===") {
            parsing = false;
            entered = true;
            appendDriveScanEntry(state, report, drives_scanned, errors_found, repairs_recommended);
            continue;
        }

        if (!parsing) {
            continue;
        }

        // Preserve the full exception text: "Error: <message>" can itself contain ':' , so
        // capture it before the generic key:value split would shred it, and so the real
        // cause reaches the report instead of being silently dropped.
        if (trimmed.startsWith(QStringLiteral("Error:"))) {
            state.error = trimmed.mid(QStringLiteral("Error:").size()).trimmed();
            continue;
        }

        QStringList parts = trimmed.split(':', Qt::SkipEmptyParts);
        if (parts.size() < kMinimumScanKeyValueParts) {
            continue;
        }

        processScanKeyValue(parts[0].trimmed(), parts[1].trimmed(), state);
    }
    return entered;
}

void CheckDiskErrorsAction::processScanKeyValue(const QString& key,
                                                const QString& value,
                                                ParsedDriveState& state) {
    if (key == "Drive") {
        state.drive_letter = value;
    } else if (key == "ScanResult") {
        state.result = value;
    } else if (key == "Status") {
        state.status = value;
    }
}

void CheckDiskErrorsAction::appendDriveScanEntry(const ParsedDriveState& state,
                                                 QString& report,
                                                 int& drives_scanned,
                                                 int& errors_found,
                                                 int& repairs_recommended) {
    // Only an answer about the volume counts as scanned. A scan that errored, whose output was cut
    // short, or that returned a result this check cannot read is never reported clean.
    const ScanVerdict verdict = classifyScanResult(state.result);
    if (verdict == ScanVerdict::Inconclusive) {
        QString detail = state.status.isEmpty() ? QStringLiteral("Scan did not complete cleanly")
                                                : state.status;
        if (!state.result.isEmpty()) {
            detail += QStringLiteral(" (result: ") + state.result + QLatin1Char(')');
        }
        if (!state.error.isEmpty()) {
            detail += QStringLiteral(" (") + state.error + QLatin1Char(')');
        }
        report += QString("Drive %1: - %2\n\n").arg(state.drive_letter).arg(detail);
        m_unchecked_drives.append(state.drive_letter + QLatin1Char(':'));
        return;
    }

    // One plain sentence per drive. Windows' own result name ("NoErrorsFound") stood above the
    // same verdict in words (review 4, Y14); it is kept only where errors call for a closer look.
    drives_scanned++;
    switch (verdict) {
    case ScanVerdict::NoErrors:
        report += QString("Drive %1: \u2713 No errors found\n").arg(state.drive_letter);
        break;
    case ScanVerdict::FixedOnline:
        errors_found++;
        report += QString(
                      "Drive %1: \u26a0 Errors found and fixed by Windows while the drive stayed "
                      "online\n")
                      .arg(state.drive_letter);
        break;
    default:
        errors_found++;
        repairs_recommended++;
        report += QString("Drive %1: \u26a0 Errors found (Windows reports %2)\n")
                      .arg(state.drive_letter, state.result);
        report += "  \u2139 Repair recommended (this check does not modify the disk)\n";
        break;
    }
    report += "\n";
}

void CheckDiskErrorsAction::executeRunVolumeScan(const QVector<QChar>& drives,
                                                 QString& report,
                                                 int& drives_scanned,
                                                 int& errors_found,
                                                 int& repairs_recommended) {
    const QDeadlineTimer run_deadline(kCheckDiskErrorsTimeoutMs);
    for (int i = 0; i < drives.count(); ++i) {
        if (isCancelled()) {
            return;
        }
        const QChar& drive = drives[i];

        const int progress = static_cast<int>(kDriveScanProgressStart +
                                              ((i * kDriveScanProgressSpan) / drives.count()));
        Q_EMIT executionProgress(QString("Scanning drive %1: for errors...").arg(drive), progress);

        const QString ps_cmd = buildScanVolumeScript(drive);

        const qint64 remaining_ms = run_deadline.remainingTime();
        if (remaining_ms == 0) {
            report +=
                QString("Drive %1: - not scanned, the run reached its time limit\n\n").arg(drive);
            m_unchecked_drives.append(QString(drive) + QLatin1Char(':'));
            continue;
        }
        const int granted_ms =
            static_cast<int>(std::min<qint64>(kDriveScanTimeoutMs, remaining_ms));
        const ProcessResult proc =
            runScanProcess(ps_cmd, granted_ms, [this]() { return isCancelled(); });
        if (proc.cancelled) {
            noteScanLeftToWindows(drive, proc.std_out);
            return;
        }
        if (proc.timed_out) {
            report += QString("Drive %1: - TIMEOUT (scan took too long)\n\n").arg(drive);
            m_unchecked_drives.append(QString(drive) + QLatin1Char(':'));
            continue;
        }
        if (!proc.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Disk scan warning for drive " + QString(drive) + ": " +
                              proc.std_err.trimmed());
        }
        // Fail closed: a scan whose process was cancelled, crashed, exited non-zero, or had
        // its output truncated is not a trustworthy result. Do not parse its stdout as a
        // successful scan -- count the drive as not scanned instead.
        if (!proc.completedSuccessfully() || proc.output_truncated) {
            report += QString("Drive %1: - scan process did not complete cleanly (%2)\n\n")
                          .arg(drive)
                          .arg(describeProcessFailure(proc));
            m_unchecked_drives.append(QString(drive) + QLatin1Char(':'));
            continue;
        }

        if (!parseDriveScanResult(
                proc.std_out, report, drives_scanned, errors_found, repairs_recommended)) {
            reportDriveWithNoResult(drive, proc.std_err, report);
        }
    }

    Q_EMIT executionProgress("Disk error check complete", progress::kComplete);
}

void CheckDiskErrorsAction::reportDriveWithNoResult(QChar drive,
                                                    const QString& std_err,
                                                    QString& report) {
    // Counted, so it is named: a run once said "1 could not be checked" and named none.
    const QString said = std_err.trimmed();
    report += QString("Drive %1: - the scan gave no result%2\n\n")
                  .arg(drive)
                  .arg(said.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(said));
    m_unchecked_drives.append(QString(drive) + QLatin1Char(':'));
}

CheckDiskErrorsAction::DiskCheckOutcome CheckDiskErrorsAction::evaluateDiskCheckOutcome(
    int drives_scanned, int total_drives) {
    const int failed = (total_drives > drives_scanned) ? (total_drives - drives_scanned) : 0;
    // Success requires every enumerated drive to have been scanned; a single
    // successful drive must not mask others that timed out or failed to scan.
    return DiskCheckOutcome{.success = total_drives > 0 && failed == 0, .drives_failed = failed};
}

void CheckDiskErrorsAction::executeBuildReport(const QDateTime& start_time,
                                               const QString& report,
                                               const DiskCheckTotals& totals) {
    const DiskCheckOutcome outcome = evaluateDiskCheckOutcome(totals.drives_scanned,
                                                              totals.total_drives);
    // A drive on a file system the online scan cannot read is named above as not checked, so it
    // is counted with the drives that could not be checked: the summary once said "0 could not be
    // checked" under a FAT32 drive it had just named. It does not fail the run.
    const auto unsupported = static_cast<int>(m_unsupported_drives.size());
    const int not_checked = outcome.drives_failed + unsupported;
    const QStringList not_checked_names = m_unchecked_drives + m_unsupported_drives;

    QString final_report = report;
    final_report += QString("-").repeated(kDiskReportWidth) + "\n";
    // One sentence a line: as one line it ran past the log window's edge, and what showed read
    // "0 could not be checked, Network drives are not checked." (review 5, Z23).
    final_report += QString(
                        "Summary: %1 of %2 scanned.\n"
                        "Could not be checked: %3.\n"
                        "%4 found, %5 recommended.\n")
                        .arg(totals.drives_scanned)
                        .arg(drivesCount(totals.total_drives + unsupported))
                        .arg(not_checked)
                        .arg(errorsCount(totals.errors_found))
                        .arg(repairsCount(totals.repairs_recommended));

    // Run as administrator, Windows may not even show this check the drives mapped for the
    // signed-in user, so this holds whether or not any network drive was named above.
    final_report += "Network drives are not checked.\n";

    if (totals.repairs_recommended > 0) {
        final_report +=
            "\n(!) Errors found -- run a disk repair to fix them "
            "(this check does not modify disks)\n";
    }

    ExecutionResult result;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.files_processed = totals.drives_scanned;
    result.success = outcome.success;

    if (totals.drives_scanned == 0) {
        result.message = "Could not scan any drives";
        // Surface the actual per-drive timeout/failure/error detail gathered above rather than
        // a guessed cause; final_report already lists why each drive did not scan.
        result.log = final_report;
    } else if (outcome.success && unsupported == 0) {
        result.message = QString("Scanned %1: %2, %3 recommended")
                             .arg(drivesCount(totals.drives_scanned))
                             .arg(errorsCount(totals.errors_found))
                             .arg(repairsCount(totals.repairs_recommended));
        result.log = final_report;
    } else {
        result.message = QString("Scanned %1 of %2; %3 could not be checked (%4)")
                             .arg(totals.drives_scanned)
                             .arg(drivesCount(totals.total_drives + unsupported))
                             .arg(not_checked)
                             .arg(not_checked_names.join(QStringLiteral(", ")));
        result.log = final_report;
    }

    finishWithResult(result, outcome.success ? ActionStatus::Success : ActionStatus::Failed);
}

}  // namespace sak
