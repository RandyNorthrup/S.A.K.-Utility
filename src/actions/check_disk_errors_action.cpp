// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file check_disk_errors_action.cpp
/// @brief Implements disk error checking using the Windows chkdsk utility

#include "sak/actions/check_disk_errors_action.h"

#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QStorageInfo>

namespace sak {

namespace {

QVector<QChar> enumerateWritableDriveLetters() {
    QVector<QChar> drives;
    const auto volumes = QStorageInfo::mountedVolumes();
    drives.reserve(volumes.size());
    for (const QStorageInfo& storage : volumes) {
        if (!storage.isValid() || storage.isReadOnly() || storage.rootPath().length() < 2) {
            continue;
        }
        QChar drive = storage.rootPath().at(0);
        if (drive.isLetter()) {
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
    result.details = "Full scan will schedule repair if corruption is detected";

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
    QDateTime start_time = QDateTime::currentDateTime();
    QVector<QChar> drives;
    QString report;
    if (!executeEnumerateVolumes(start_time, drives, report)) {
        return;
    }

    int drives_scanned = 0;
    int errors_found = 0;
    int errors_fixed = 0;
    executeRunChkdsk(drives, report, drives_scanned, errors_found, errors_fixed);

    executeBuildReport(start_time, report, drives_scanned, errors_found, errors_fixed);
}

bool CheckDiskErrorsAction::executeEnumerateVolumes(const QDateTime& start_time,
                                                    QVector<QChar>& drives,
                                                    QString& report) {
    Q_EMIT executionProgress("Detecting disk drives...", 5);

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

    report += "+" + QString("=").repeated(78) + "+\n";
    report += "|" + QString(" DISK ERROR CHECK & REPAIR REPORT").leftJustified(78) + "|\n";
    report += "+" + QString("=").repeated(78) + "+\n\n";

    return true;
}

QString CheckDiskErrorsAction::buildRepairVolumeScript(QChar drive) {
    return QString(
               "$drive = \"%1:\"\\n"
               "Write-Output '===SCAN_START==='\\n"
               "Write-Output \"Drive: $drive\"\\n"
               "\\n"
               "try {\\n"
               "    Write-Output 'Running online scan...'\\n"
               "    Repair-Volume -DriveLetter %1 -Scan -ErrorAction Stop\\n"
               "    Write-Output 'OnlineScan: Success'\\n"
               "    \\n"
               "    if (Test-Path \"$drive\\\\`$corrupt\") {\\n"
               "        Write-Output 'CorruptFile: Detected'\\n"
               "        Write-Output 'Status: Corruption detected - offline repair needed'\\n"
               "        Write-Output 'Scheduling offline repair...'\\n"
               "        Repair-Volume -DriveLetter %1 -OfflineScanAndFix -ErrorAction Stop\\n"
               "        Write-Output 'OfflineRepair: Scheduled'\\n"
               "        Write-Output 'RebootRequired: Yes'\\n"
               "    } else {\\n"
               "        Write-Output 'CorruptFile: NotFound'\\n"
               "        Write-Output 'Status: No corruption detected'\\n"
               "        Write-Output 'RebootRequired: No'\\n"
               "    }\\n"
               "} catch {\\n"
               "    Write-Output \"Error: $($_.Exception.Message)\"\\n"
               "    Write-Output 'Status: Scan failed'\\n"
               "}\\n"
               "\\n"
               "Write-Output '===SCAN_END==='\\n")
        .arg(drive);
}

void CheckDiskErrorsAction::parseDriveScanResult(const QString& output,
                                                 QString& report,
                                                 int& drives_scanned,
                                                 int& errors_found,
                                                 int& errors_fixed) {
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    bool parsing = false;
    ParsedDriveState state;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();

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

        QStringList parts = trimmed.split(':', Qt::SkipEmptyParts);
        if (parts.size() < 2) {
            continue;
        }

        processScanKeyValue(parts[0].trimmed(), parts[1].trimmed(), state, errors_fixed);
    }
}

void CheckDiskErrorsAction::processScanKeyValue(const QString& key,
                                                const QString& value,
                                                ParsedDriveState& state,
                                                int& errors_fixed) {
    if (key == "Drive") {
        state.drive_letter = value;
    } else if (key == "OnlineScan") {
        state.scan_success = (value == "Success");
    } else if (key == "CorruptFile") {
        state.has_corrupt = (value == "Detected");
    } else if (key == "Status") {
        state.status = value;
    } else if (key == "RebootRequired") {
        state.reboot_needed = (value == "Yes");
    } else if (key == "OfflineRepair" && value == "Scheduled") {
        errors_fixed++;
    }
}

void CheckDiskErrorsAction::appendDriveScanEntry(const ParsedDriveState& state,
                                                 QString& report,
                                                 int& drives_scanned,
                                                 int& errors_found) {
    if (state.scan_success) {
        drives_scanned++;

        report += QString("Drive %1:\n").arg(state.drive_letter);
        report += QString("  Status: %1\n").arg(state.status);

        if (state.has_corrupt) {
            errors_found++;
            report += "  \u26a0 Corruption detected: $corrupt file found\n";
            report += "  \u2139 Offline repair scheduled at next reboot\n";
        } else {
            report += "  \u2713 No corruption detected\n";
        }

        if (state.reboot_needed) {
            report += "  \u26a0 REBOOT REQUIRED to complete repair\n";
        }
    } else {
        report += QString("Drive %1: - %2\n").arg(state.drive_letter).arg(state.status);
    }

    report += "\n";
}

void CheckDiskErrorsAction::executeRunChkdsk(const QVector<QChar>& drives,
                                             QString& report,
                                             int& drives_scanned,
                                             int& errors_found,
                                             int& errors_fixed) {
    for (int i = 0; i < drives.count(); ++i) {
        const QChar& drive = drives[i];

        int progress = 10 + ((i * 80) / drives.count());
        Q_EMIT executionProgress(QString("Scanning drive %1: with Repair-Volume...").arg(drive),
                                 progress);

        QString ps_cmd = buildRepairVolumeScript(drive);

        ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessLongMs);
        if (proc.timed_out) {
            report += QString("Drive %1: - TIMEOUT (scan took too long)\n\n").arg(drive);
            continue;
        }
        if (!proc.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Disk scan warning for drive " + QString(drive) + ": " +
                              proc.std_err.trimmed());
        }

        parseDriveScanResult(proc.std_out, report, drives_scanned, errors_found, errors_fixed);
    }

    Q_EMIT executionProgress("Disk error check complete", 100);
}

void CheckDiskErrorsAction::executeBuildReport(const QDateTime& start_time,
                                               const QString& report,
                                               int drives_scanned,
                                               int errors_found,
                                               int errors_fixed) {
    QString final_report = report;
    final_report += QString("-").repeated(78) + "\n";
    final_report += QString(
                        "Summary: %1 drive(s) scanned, %2 error(s) found, %3 repair(s) "
                        "scheduled\n")
                        .arg(drives_scanned)
                        .arg(errors_found)
                        .arg(errors_fixed);

    if (errors_fixed > 0) {
        final_report += "\n(!) REBOOT REQUIRED to complete offline disk repair\n";
    }

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = drives_scanned;

    if (drives_scanned > 0) {
        result.success = true;
        result.message = QString("Scanned %1 drive(s): %2 error(s), %3 repair(s) scheduled")
                             .arg(drives_scanned)
                             .arg(errors_found)
                             .arg(errors_fixed);
        result.log = final_report;
    } else {
        result.success = false;
        result.message = "Could not scan any drives";
        result.log =
            "No drives scanned or PowerShell Storage module unavailable (requires admin "
            "privileges)";
    }

    finishWithResult(result, drives_scanned > 0 ? ActionStatus::Success : ActionStatus::Failed);
}

}  // namespace sak
