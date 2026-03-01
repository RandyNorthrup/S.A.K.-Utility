#!/usr/bin/env python3
"""Regenerate check_disk_errors and windows_update fragments with correct escaping."""
import os

FRAG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fragments")

# =====================================================================
# check_disk_errors.cpp - must use \\n in PS script, exact Unicode chars
# =====================================================================
chk = []
chk.append('''void CheckDiskErrorsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Disk error check cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    QVector<QChar> drives;
    QString report;
    if (!executeEnumerateVolumes(start_time, drives, report)) return;

    int drives_scanned = 0;
    int errors_found = 0;
    int errors_fixed = 0;
    executeRunChkdsk(drives, report, drives_scanned, errors_found, errors_fixed);

    executeBuildReport(start_time, report, drives_scanned, errors_found, errors_fixed);
}

bool CheckDiskErrorsAction::executeEnumerateVolumes(const QDateTime& start_time,
                                                     QVector<QChar>& drives,
                                                     QString& report)
{
    Q_EMIT executionProgress("Detecting disk drives...", 5);

    // Get all drives
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (storage.isValid() && !storage.isReadOnly() && storage.rootPath().length() >= 2) {
            QChar drive = storage.rootPath().at(0);
            if (drive.isLetter()) {
                drives.append(drive);
            }
        }
    }

    if (drives.isEmpty()) {
        ExecutionResult result;
        result.success = false;
        result.message = "No valid drives found for scanning";
        result.log = "Unable to detect any readable, writable volumes";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        finishWithResult(result, ActionStatus::Failed);
        return false;
    }

''')
chk.append('    report += "\u2554" + QString("\u2550").repeated(78) + "\u2557\\n";\n')
chk.append('    report += "\u2551" + QString(" DISK ERROR CHECK & REPAIR REPORT").leftJustified(78) + "\u2551\\n";\n')
chk.append('    report += "\u255a" + QString("\u2550").repeated(78) + "\u255d\\n\\n";\n')
chk.append('''
    return true;
}

QString CheckDiskErrorsAction::buildRepairVolumeScript(QChar drive)
{
    return QString(
''')
# PS script lines - must use \\n (literal backslash-n in C++ string)
chk.append('        "$drive = \\"%1:\\"\\\\n"\n')
chk.append("        \"Write-Output '===SCAN_START==='\\\\n\"\n")
chk.append('        "Write-Output \\"Drive: $drive\\"\\\\n"\n')
chk.append('        "\\\\n"\n')
chk.append('        "try {\\\\n"\n')
chk.append("        \"    Write-Output 'Running online scan...'\\\\n\"\n")
chk.append('        "    Repair-Volume -DriveLetter %1 -Scan -ErrorAction Stop\\\\n"\n')
chk.append("        \"    Write-Output 'OnlineScan: Success'\\\\n\"\n")
chk.append('        "    \\\\n"\n')
chk.append('        "    if (Test-Path \\"$drive\\\\\\\\`$corrupt\\") {\\\\n"\n')
chk.append("        \"        Write-Output 'CorruptFile: Detected'\\\\n\"\n")
chk.append("        \"        Write-Output 'Status: Corruption detected - offline repair needed'\\\\n\"\n")
chk.append("        \"        Write-Output 'Scheduling offline repair...'\\\\n\"\n")
chk.append('        "        Repair-Volume -DriveLetter %1 -OfflineScanAndFix -ErrorAction Stop\\\\n"\n')
chk.append("        \"        Write-Output 'OfflineRepair: Scheduled'\\\\n\"\n")
chk.append("        \"        Write-Output 'RebootRequired: Yes'\\\\n\"\n")
chk.append('        "    } else {\\\\n"\n')
chk.append("        \"        Write-Output 'CorruptFile: NotFound'\\\\n\"\n")
chk.append("        \"        Write-Output 'Status: No corruption detected'\\\\n\"\n")
chk.append("        \"        Write-Output 'RebootRequired: No'\\\\n\"\n")
chk.append('        "    }\\\\n"\n')
chk.append('        "} catch {\\\\n"\n')
chk.append('        "    Write-Output \\"Error: $($_.Exception.Message)\\"\\\\n"\n')
chk.append("        \"    Write-Output 'Status: Scan failed'\\\\n\"\n")
chk.append('        "}\\\\n"\n')
chk.append('        "\\\\n"\n')
chk.append("        \"Write-Output '===SCAN_END==='\\\\n\"\n")
chk.append('''    ).arg(drive);
}

void CheckDiskErrorsAction::parseDriveScanResult(const QString& output, QChar drive,
                                                   QString& report, int& drives_scanned,
                                                   int& errors_found, int& errors_fixed)
{
    QStringList lines = output.split('\\n', Qt::SkipEmptyParts);

    bool parsing = false;
    QString drive_letter;
    QString status;
    bool has_corrupt = false;
    bool reboot_needed = false;
    bool scan_success = false;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();

        if (trimmed == "===SCAN_START===") {
            parsing = true;
            continue;
        }

        if (trimmed == "===SCAN_END===") {
            parsing = false;

            if (scan_success) {
                drives_scanned++;

                report += QString("Drive %1:\\n").arg(drive_letter);
                report += QString("  Status: %1\\n").arg(status);

                if (has_corrupt) {
                    errors_found++;
''')
chk.append('                    report += "  \u26a0 Corruption detected: $corrupt file found\\n";\n')
chk.append('                    report += "  \u2139 Offline repair scheduled at next reboot\\n";\n')
chk.append('''                    reboot_needed = true;
                } else {
''')
chk.append('                    report += "  \u2713 No corruption detected\\n";\n')
chk.append('''                }

                if (reboot_needed) {
''')
chk.append('                    report += "  \u26a0 REBOOT REQUIRED to complete repair\\n";\n')
chk.append('''                }
            } else {
                report += QString("Drive %1: - %2\\n").arg(drive_letter).arg(status);
            }

            report += "\\n";
            continue;
        }

        if (!parsing) continue;

        QStringList parts = trimmed.split(':', Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;

        QString key = parts[0].trimmed();
        QString value = parts[1].trimmed();

        if (key == "Drive") {
            drive_letter = value;
        } else if (key == "OnlineScan" && value == "Success") {
            scan_success = true;
        } else if (key == "CorruptFile" && value == "Detected") {
            has_corrupt = true;
        } else if (key == "Status") {
            status = value;
        } else if (key == "RebootRequired" && value == "Yes") {
            reboot_needed = true;
        } else if (key == "OfflineRepair" && value == "Scheduled") {
            errors_fixed++;
        }
    }
}

void CheckDiskErrorsAction::executeRunChkdsk(const QVector<QChar>& drives, QString& report,
                                               int& drives_scanned, int& errors_found,
                                               int& errors_fixed)
{
    for (int i = 0; i < drives.count(); ++i) {
        const QChar& drive = drives[i];

        int progress = 10 + ((i * 80) / drives.count());
        Q_EMIT executionProgress(QString("Scanning drive %1: with Repair-Volume...").arg(drive), progress);

        QString ps_cmd = buildRepairVolumeScript(drive);

        ProcessResult proc = runPowerShell(ps_cmd, 30000);
        if (proc.timed_out) {
            report += QString("Drive %1: - TIMEOUT (scan took too long)\\n\\n").arg(drive);
            continue;
        }
        if (!proc.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Disk scan warning for drive " + QString(drive) + ": " + proc.std_err.trimmed());
        }

        parseDriveScanResult(proc.std_out, drive, report, drives_scanned, errors_found, errors_fixed);
    }

    Q_EMIT executionProgress("Disk error check complete", 100);
}

void CheckDiskErrorsAction::executeBuildReport(const QDateTime& start_time,
                                                const QString& report,
                                                int drives_scanned,
                                                int errors_found,
                                                int errors_fixed)
{
    QString final_report = report;
''')
chk.append('    final_report += QString("\u2500").repeated(78) + "\\n";\n')
chk.append('''    final_report += QString("Summary: %1 drive(s) scanned, %2 error(s) found, %3 repair(s) scheduled\\n")
        .arg(drives_scanned).arg(errors_found).arg(errors_fixed);

    if (errors_fixed > 0) {
''')
chk.append('        final_report += "\\n\u26a0 REBOOT REQUIRED to complete offline disk repair\\n";\n')
chk.append('''    }

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = drives_scanned;

    if (drives_scanned > 0) {
        result.success = true;
        result.message = QString("Scanned %1 drive(s): %2 error(s), %3 repair(s) scheduled")
            .arg(drives_scanned).arg(errors_found).arg(errors_fixed);
        result.log = final_report;
    } else {
        result.success = false;
        result.message = "Could not scan any drives";
        result.log = "No drives scanned or PowerShell Storage module unavailable (requires admin privileges)";
    }

    finishWithResult(result, drives_scanned > 0 ? ActionStatus::Success : ActionStatus::Failed);
}
''')

with open(os.path.join(FRAG, "check_disk_errors.cpp"), 'w', encoding='utf-8') as f:
    f.write(''.join(chk))
print("  Wrote check_disk_errors.cpp")

# =====================================================================
# windows_update.cpp - fix \' escaping
# =====================================================================
wu_path = os.path.join(FRAG, "windows_update.cpp")
with open(wu_path, 'r', encoding='utf-8') as f:
    wu = f.read()

# Fix \\' to \' (original uses \' not \\')
wu = wu.replace("\\\\'Software\\\\'", "\\'Software\\'")

with open(wu_path, 'w', encoding='utf-8') as f:
    f.write(wu)
print("  Fixed windows_update.cpp")

print("Done!")
