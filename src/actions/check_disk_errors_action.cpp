// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file check_disk_errors_action.cpp
/// @brief Implements disk error checking using the Windows chkdsk utility

#include "sak/actions/check_disk_errors_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QStorageInfo>

namespace sak {

namespace {
constexpr int kBareDriveRootLength = 3;      ///< "C:/" -- the only form Repair-Volume can address.
constexpr int kDriveRootSeparatorIndex = 2;  ///< index of the path separator in "C:/".
constexpr int kDiskReportWidth = 78;
constexpr int kMinimumScanKeyValueParts = 2;
constexpr int kDriveScanProgressStart = 10;
constexpr int kDriveScanProgressSpan = 80;

QVector<QChar> enumerateWritableDriveLetters() {
    QVector<QChar> drives;
    const auto volumes = QStorageInfo::mountedVolumes();
    drives.reserve(volumes.size());
    for (const QStorageInfo& storage : volumes) {
        if (!storage.isValid() || storage.isReadOnly()) {
            continue;
        }
        // Only a volume mounted at a bare drive-letter root ("C:/") can be addressed by
        // Repair-Volume -DriveLetter. A directory-mounted volume ("C:/Mount/Foreign/") would
        // otherwise be misread as its mount path's first letter and the host drive scanned in
        // its place -- fail closed by skipping anything that is not exactly "<letter>:<sep>",
        // and de-duplicate so one drive is never scanned (or counted) twice.
        const QString root = storage.rootPath();
        if (root.length() != kBareDriveRootLength || !root.at(0).isLetter() ||
            root.at(1) != QLatin1Char(':') ||
            (root.at(kDriveRootSeparatorIndex) != QLatin1Char('/') &&
             root.at(kDriveRootSeparatorIndex) != QLatin1Char('\\'))) {
            continue;
        }
        const QChar drive = root.at(0).toUpper();
        if (!drives.contains(drive)) {
            drives.append(drive);
        }
    }
    return drives;
}

}  // namespace

CheckDiskErrorsAction::CheckDiskErrorsAction(QObject* parent) : QuickAction(parent) {}

void CheckDiskErrorsAction::scan() {
    setStatus(ActionStatus::Scanning);

    const QVector<QChar> drives = enumerateWritableDriveLetters();

    ScanResult result;
    result.applicable = !drives.isEmpty();
    result.summary = result.applicable ? QString("Drives detected: %1").arg(drives.count())
                                       : "No writable drives detected";
    result.details = "Read-only online scan; reports drives needing repair without modifying them";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void CheckDiskErrorsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Disk error check cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    const QDateTime start_time = QDateTime::currentDateTime();
    QVector<QChar> drives;
    QString report;
    if (!executeEnumerateVolumes(start_time, drives, report)) {
        return;
    }

    DiskCheckTotals totals;
    totals.total_drives = static_cast<int>(drives.count());
    executeRunChkdsk(
        drives, report, totals.drives_scanned, totals.errors_found, totals.repairs_recommended);

    executeBuildReport(start_time, report, totals);
}

bool CheckDiskErrorsAction::executeEnumerateVolumes(const QDateTime& start_time,
                                                    QVector<QChar>& drives,
                                                    QString& report) {
    Q_EMIT executionProgress("Detecting disk drives...", progress::kStep5);

    drives = enumerateWritableDriveLetters();

    if (drives.isEmpty()) {
        ExecutionResult result;
        result.success = false;
        result.message = "No valid drives found for scanning";
        result.log = "Unable to detect any readable, writable volumes";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        finishWithResult(result, ActionStatus::Failed);
        return false;
    }

    report += "+" + QString("=").repeated(kDiskReportWidth) + "+\n";
    report += "|" + QString(" DISK ERROR CHECK & REPAIR REPORT").leftJustified(kDiskReportWidth) +
              "|\n";
    report += "+" + QString("=").repeated(kDiskReportWidth) + "+\n\n";

    return true;
}

QString CheckDiskErrorsAction::buildScanVolumeScript(QChar drive) {
    // Read-only check ONLY: Repair-Volume -Scan is a non-destructive online scan.
    // This action is a "Check Disk Errors" scan and must NOT mutate the disk, so
    // it never calls -OfflineScanAndFix (which schedules a boot-time repair). On
    // detecting the $corrupt marker it merely reports that a repair is advisable.
    return QString(
               "$drive = \"%1:\"\n"
               "Write-Output '===SCAN_START==='\n"
               "Write-Output \"Drive: $drive\"\n"
               "\n"
               "try {\n"
               "    Write-Output 'Running online scan...'\n"
               "    Repair-Volume -DriveLetter %1 -Scan -ErrorAction Stop\n"
               "    Write-Output 'OnlineScan: Success'\n"
               "    \n"
               "    if (Test-Path \"$drive\\\\`$corrupt\" -ErrorAction Stop) {\n"
               "        Write-Output 'CorruptFile: Detected'\n"
               "        Write-Output 'Status: Corruption detected - repair recommended'\n"
               "        Write-Output 'RepairRecommended: Yes'\n"
               "    } else {\n"
               "        Write-Output 'CorruptFile: NotFound'\n"
               "        Write-Output 'Status: No corruption detected'\n"
               "        Write-Output 'RepairRecommended: No'\n"
               "    }\n"
               "} catch {\n"
               "    Write-Output \"Error: $($_.Exception.Message)\"\n"
               "    Write-Output 'Status: Scan failed'\n"
               "}\n"
               "\n"
               "Write-Output '===SCAN_END==='\n")
        .arg(drive);
}

void CheckDiskErrorsAction::parseDriveScanResult(const QString& output,
                                                 QString& report,
                                                 int& drives_scanned,
                                                 int& errors_found,
                                                 int& repairs_recommended) {
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    bool parsing = false;
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
            appendDriveScanEntry(state, report, drives_scanned, errors_found);
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

        processScanKeyValue(parts[0].trimmed(), parts[1].trimmed(), state, repairs_recommended);
    }
}

void CheckDiskErrorsAction::processScanKeyValue(const QString& key,
                                                const QString& value,
                                                ParsedDriveState& state,
                                                int& repairs_recommended) {
    if (key == "Drive") {
        state.drive_letter = value;
    } else if (key == "OnlineScan") {
        state.scan_success = (value == "Success");
    } else if (key == "CorruptFile") {
        // Only a recognized verdict counts as known; a malformed value leaves corrupt_known
        // false so the drive is not reported clean on an ambiguous result.
        if (value == "Detected") {
            state.has_corrupt = true;
            state.corrupt_known = true;
        } else if (value == "NotFound") {
            state.has_corrupt = false;
            state.corrupt_known = true;
        }
    } else if (key == "Status") {
        state.status = value;
    } else if (key == "RepairRecommended" && value == "Yes") {
        repairs_recommended++;
    }
}

void CheckDiskErrorsAction::appendDriveScanEntry(const ParsedDriveState& state,
                                                 QString& report,
                                                 int& drives_scanned,
                                                 int& errors_found) {
    // A drive is only a successful scan when the online scan reported success AND a definitive
    // corruption verdict was seen. A scan whose corruption check errored or whose output was
    // cut short after "OnlineScan: Success" leaves corrupt_known false and must NOT be reported
    // clean -- fail closed and count it as not scanned.
    if (state.scan_success && state.corrupt_known) {
        drives_scanned++;

        report += QString("Drive %1:\n").arg(state.drive_letter);
        report += QString("  Status: %1\n").arg(state.status);

        if (state.has_corrupt) {
            errors_found++;
            report += "  \u26a0 Corruption detected: $corrupt file found\n";
            report += "  \u2139 Repair recommended (this check does not modify the disk)\n";
        } else {
            report += "  \u2713 No corruption detected\n";
        }
    } else {
        QString detail = state.status.isEmpty() ? QStringLiteral("Scan did not complete cleanly")
                                                : state.status;
        if (!state.error.isEmpty()) {
            detail += QStringLiteral(" (") + state.error + QLatin1Char(')');
        }
        report += QString("Drive %1: - %2\n").arg(state.drive_letter).arg(detail);
    }

    report += "\n";
}

void CheckDiskErrorsAction::executeRunChkdsk(const QVector<QChar>& drives,
                                             QString& report,
                                             int& drives_scanned,
                                             int& errors_found,
                                             int& repairs_recommended) {
    for (int i = 0; i < drives.count(); ++i) {
        const QChar& drive = drives[i];

        const int progress = kDriveScanProgressStart +
                             ((i * kDriveScanProgressSpan) / drives.count());
        Q_EMIT executionProgress(QString("Scanning drive %1: for errors...").arg(drive), progress);

        const QString ps_cmd = buildScanVolumeScript(drive);

        const ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessLongMs);
        if (proc.timed_out) {
            report += QString("Drive %1: - TIMEOUT (scan took too long)\n\n").arg(drive);
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
            report += QString("Drive %1: - scan process did not complete cleanly\n\n").arg(drive);
            continue;
        }

        parseDriveScanResult(
            proc.std_out, report, drives_scanned, errors_found, repairs_recommended);
    }

    Q_EMIT executionProgress("Disk error check complete", progress::kComplete);
}

CheckDiskErrorsAction::DiskCheckOutcome CheckDiskErrorsAction::evaluateDiskCheckOutcome(
    int drives_scanned, int total_drives) {
    const int failed = (total_drives > drives_scanned) ? (total_drives - drives_scanned) : 0;
    // Success requires every enumerated drive to have been scanned; a single
    // successful drive must not mask others that timed out or failed to scan.
    return DiskCheckOutcome{total_drives > 0 && failed == 0, failed};
}

void CheckDiskErrorsAction::executeBuildReport(const QDateTime& start_time,
                                               const QString& report,
                                               const DiskCheckTotals& totals) {
    const DiskCheckOutcome outcome = evaluateDiskCheckOutcome(totals.drives_scanned,
                                                              totals.total_drives);

    QString final_report = report;
    final_report += QString("-").repeated(kDiskReportWidth) + "\n";
    final_report += QString(
                        "Summary: %1 of %2 drive(s) scanned, %3 could not be checked, "
                        "%4 error(s) found, %5 repair(s) recommended\n")
                        .arg(totals.drives_scanned)
                        .arg(totals.total_drives)
                        .arg(outcome.drives_failed)
                        .arg(totals.errors_found)
                        .arg(totals.repairs_recommended);

    if (totals.repairs_recommended > 0) {
        final_report +=
            "\n(!) Corruption found -- run a disk repair to fix "
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
    } else if (outcome.success) {
        result.message = QString("Scanned %1 drive(s): %2 error(s), %3 repair(s) recommended")
                             .arg(totals.drives_scanned)
                             .arg(totals.errors_found)
                             .arg(totals.repairs_recommended);
        result.log = final_report;
    } else {
        result.message = QString("Scanned %1 of %2 drive(s); %3 could not be checked")
                             .arg(totals.drives_scanned)
                             .arg(totals.total_drives)
                             .arg(outcome.drives_failed);
        result.log = final_report;
    }

    finishWithResult(result, outcome.success ? ActionStatus::Success : ActionStatus::Failed);
}

}  // namespace sak
