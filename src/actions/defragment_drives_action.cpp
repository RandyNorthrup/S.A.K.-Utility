// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file defragment_drives_action.cpp
/// @brief Implements drive defragmentation and optimization analysis

#include "sak/actions/defragment_drives_action.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"
#include <QStorageInfo>
#include <QRegularExpression>

namespace sak {

DefragmentDrivesAction::DefragmentDrivesAction(QObject* parent)
    : QuickAction(parent)
{
}

bool DefragmentDrivesAction::isDriveSSD(const QString& drive_letter) {
    QString cmd = QString("Get-PhysicalDisk | Where-Object {$_.DeviceID -eq (Get-Partition "
                          "-DriveLetter %1).DiskNumber} | Select-Object -ExpandProperty MediaType")
                     .arg(drive_letter.left(1));
    ProcessResult proc = runPowerShell(cmd, sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Drive media type warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out.trimmed();
    return output.contains("SSD", Qt::CaseInsensitive);
}

int DefragmentDrivesAction::analyzeFragmentation(const QString& drive_letter) {
    QString cmd = QString("defrag %1: /A").arg(drive_letter);
    ProcessResult proc = runProcess("cmd.exe", QStringList() << "/c" << cmd,
        sak::kTimeoutProcessLongMs);
    QString output = proc.std_out;

    // Parse fragmentation percentage from output
    QRegularExpression re("(\\d+)%.*fragmented");
    QRegularExpressionMatch match = re.match(output);

    if (match.hasMatch()) {
        bool ok = false;
        int fragmentation = match.captured(1).toInt(&ok);
        return ok ? fragmentation : 0;
    }
    return 0;
}

void DefragmentDrivesAction::scan() {
    setStatus(ActionStatus::Scanning);

    Q_EMIT scanProgress("Enumerating fixed drives...");

    int fixed_drives = 0;
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()) continue;
        if (storage.rootPath().isEmpty() || storage.rootPath().length() < 2) continue;
        fixed_drives++;
    }

    ScanResult result;
    result.applicable = fixed_drives > 0;
    result.summary = fixed_drives > 0
        ? QString("Fixed drives detected: %1").arg(fixed_drives)
        : "No fixed drives detected";
    result.details = "Optimization uses Optimize-Volume (defrag/TRIM based on media type)";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void DefragmentDrivesAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_EMIT executionProgress("Analyzing drives for optimization...", 5);

    QString ps_script = executeEnumerateVolumes();
    executeDefrag(ps_script, start_time);
}

QString DefragmentDrivesAction::executeEnumerateVolumes() const {
    // Enterprise approach: Use Optimize-Volume PowerShell cmdlet
    // Per Microsoft docs: Automatically selects correct optimization per drive type:
    // - HDD: Defragmentation
    // - SSD with TRIM: TRIM/Retrim operation
    // - Tiered Storage: TierOptimize

    return
        "# Enterprise Drive Optimization using Optimize-Volume\n"
        "$ErrorActionPreference = 'Continue'; \n"
        "\n"
        "# Get all fixed volumes\n"
        "$volumes = Get-Volume | Where-Object { \n"
        "    $_.DriveType -eq 'Fixed' -and \n"
        "    $_.DriveLetter -and \n"
        "    $_.FileSystem -eq 'NTFS' \n"
        "}; \n"
        "\n"
        "if ($volumes.Count -eq 0) { \n"
        "    Write-Output 'NO_DRIVES_FOUND'; \n"
        "    exit 0; \n"
        "} \n"
        "\n"
        "Write-Output \"Found $($volumes.Count) drive(s) to optimize\"; \n"
        "\n"
        "$optimized = 0; \n"
        "$skipped = 0; \n"
        "\n"
        "foreach ($volume in $volumes) { \n"
        "    $driveLetter = $volume.DriveLetter; \n"
        "    Write-Output \"OPTIMIZING:$driveLetter\"; \n"
        "    \n"
        "    try { \n"
        "        # Get drive type using Get-PhysicalDisk\n"
        "        $partition = Get-Partition -DriveLetter $driveLetter -ErrorAction "
        "SilentlyContinue; \n"
        "        $disk = Get-PhysicalDisk -ErrorAction SilentlyContinue | Where-Object { \n"
        "            $_.DeviceID -eq $partition.DiskNumber \n"
        "        } | Select-Object -First 1; \n"
        "        \n"
        "        $driveType = if ($disk) { $disk.MediaType } else { 'Unknown' }; \n"
        "        Write-Output \"DRIVE_TYPE:$driveLetter=$driveType\"; \n"
        "        \n"
        "        # Optimize-Volume automatically selects correct operation:\n"
        "        # HDD -> Defrag, SSD -> Retrim, Tiered -> TierOptimize\n"
        "        Write-Output \"Starting optimization for $driveLetter`:...\"; \n"
        "        \n"
        "        # Run optimization with verbose output\n"
        "        Optimize-Volume -DriveLetter $driveLetter -Verbose -ErrorAction Stop; \n"
        "        \n"
        "        Write-Output \"SUCCESS:$driveLetter\"; \n"
        "        $optimized++; \n"
        "    } catch { \n"
        "        if ($_.Exception.Message -match 'not supported') { \n"
        "            Write-Output \"SKIPPED:$driveLetter (optimization not needed)\"; \n"
        "            $skipped++; \n"
        "        } else { \n"
        "            Write-Warning \"ERROR:$driveLetter - $($_.Exception.Message)\"; \n"
        "        } \n"
        "    } \n"
        "} \n"
        "\n"
        "Write-Output \"TOTAL_OPTIMIZED:$optimized\"; \n"
        "Write-Output \"TOTAL_SKIPPED:$skipped\"; \n"
        "Write-Output 'COMPLETE'";
}

void DefragmentDrivesAction::executeDefrag(const QString& ps_script, const QDateTime& start_time) {
    Q_EMIT executionProgress("Optimizing drives...", 15);

    ProcessResult ps_result = runPowerShell(ps_script, sak::kTimeoutDefragMs);
    if (ps_result.timed_out || isCancelled()) {
        emitCancelledResult(QStringLiteral("Drive optimization cancelled"), start_time);
        return;
    }

    executeBuildReport(ps_result.std_out, ps_result.std_err, start_time);
}

void DefragmentDrivesAction::executeBuildReport(
        const QString& accumulated_output, const QString& std_err,
        const QDateTime& start_time) {

    auto summary = parseOptimizationOutput(accumulated_output);

    if (accumulated_output.contains("NO_DRIVES_FOUND")) {
        ExecutionResult result;
        result.success = true;
        result.message = "No fixed NTFS drives found to optimize";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.log = accumulated_output + (std_err.isEmpty() ? "" : "\nErrors:\n" + std_err);
        finishWithResult(result, ActionStatus::Success);
        return;
    }

    Q_EMIT executionProgress("Optimization complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.success = true;

    QString drive_info = summary.drive_types.join("\n");

    if (summary.total_optimized > 0) {
        result.message = QString("Optimized %1 drive(s)").arg(summary.total_optimized);
        if (summary.total_skipped > 0) {
            result.message += QString(" (%1 skipped)").arg(summary.total_skipped);
        }
    } else if (summary.total_skipped > 0) {
        result.message = QString("All %1 drive(s) skipped (no optimization needed)")
            .arg(summary.total_skipped);
    } else {
        result.message = "Drive optimization completed";
    }

    result.log = QString("Drive Types:\n%1\n\nOptimization Details:\n%2")
                    .arg(drive_info)
                    .arg(accumulated_output);
    if (!std_err.trimmed().isEmpty()) {
        result.log += "\nErrors:\n" + std_err.trimmed();
    }

    finishWithResult(result, ActionStatus::Success);
}

DefragmentDrivesAction::OptimizationSummary DefragmentDrivesAction::parseOptimizationOutput(
        const QString& output) const
{
    OptimizationSummary summary;

    QRegularExpression optRe("OPTIMIZING:([A-Z])");
    auto optIt = optRe.globalMatch(output);
    while (optIt.hasNext()) {
        optIt.next();
        summary.total_drives++;
    }

    QRegularExpression typeRe("DRIVE_TYPE:([A-Z])=(.+)");
    auto typeIt = typeRe.globalMatch(output);
    while (typeIt.hasNext()) {
        auto match = typeIt.next();
        summary.drive_types.append(QString("%1: = %2").arg(match.captured(1),
            match.captured(2).trimmed()));
    }

    summary.optimized = output.count("SUCCESS:", Qt::CaseInsensitive);

    QRegularExpression optimizedRe("TOTAL_OPTIMIZED:(\\d+)");
    QRegularExpression skippedRe("TOTAL_SKIPPED:(\\d+)");

    QRegularExpressionMatch optMatch = optimizedRe.match(output);
    QRegularExpressionMatch skipMatch = skippedRe.match(output);

    summary.total_optimized =
        optMatch.hasMatch() ? optMatch.captured(1).toInt() : summary.optimized;
    summary.total_skipped = skipMatch.hasMatch() ? skipMatch.captured(1).toInt() : 0;

    return summary;
}

} // namespace sak
