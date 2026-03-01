// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file disk_cleanup_action.cpp
/// @brief Implements comprehensive disk space cleanup across system directories

#include "sak/actions/disk_cleanup_action.h"
#include "sak/path_utils.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDateTime>
#include "sak/layout_constants.h"
#include "sak/logger.h"

namespace sak {

DiskCleanupAction::DiskCleanupAction()
    : QuickAction(nullptr) {
}

void DiskCleanupAction::scan() {
    setStatus(ActionStatus::Scanning);
    m_targets.clear();
    m_total_bytes = 0;
    m_total_files = 0;

    Q_EMIT scanProgress("Scanning Windows temp files...");
    scanWindowsTemp();
    if (isCancelled()) {
        return;
    }

    Q_EMIT scanProgress("Scanning user temp files...");
    scanUserTemp();
    if (isCancelled()) {
        return;
    }

    Q_EMIT scanProgress("Scanning browser caches...");
    scanBrowserCaches();
    if (isCancelled()) {
        return;
    }

    Q_EMIT scanProgress("Scanning recycle bin...");
    scanRecycleBin();
    if (isCancelled()) {
        return;
    }

    Q_EMIT scanProgress("Scanning Windows Update cache...");
    scanWindowsUpdate();
    if (isCancelled()) {
        return;
    }

    Q_EMIT scanProgress("Scanning thumbnail cache...");
    scanThumbnailCache();

    ScanResult result;
    result.applicable = m_total_bytes > 0;
    result.bytes_affected = m_total_bytes;
    result.files_count = m_total_files;
    result.estimated_duration_ms = std::max<qint64>(5000, m_total_files * 5);

    if (result.applicable) {
        double mb = m_total_bytes / sak::kBytesPerMBf;
        result.summary = QString("Potential cleanup: %1 MB").arg(mb, 0, 'f', 1);
        result.details = QString("Targets: %1, Files: %2")
            .arg(m_targets.size())
            .arg(m_total_files);
    } else {
        result.summary = "No cleanup targets found";
        result.details = "System appears clean";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void DiskCleanupAction::execute() {
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("Disk cleanup cancelled"));
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    QStringList drives;
    QString drives_error;
    if (!executeCalculateSpace(drives, drives_error, start_time)) return;

    const int PROFILE_ID = 5432;
    QString sagerun_arg = QString("/sagerun:%1").arg(PROFILE_ID);
    int drives_processed = 0;
    qint64 total_freed = 0;
    executeCleanup(drives, sagerun_arg, drives_processed, total_freed);

    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("Cleanup cancelled by user"), start_time);
        return;
    }

    executeBuildReport(drives_processed, total_freed, drives_error, start_time);
}

bool DiskCleanupAction::executeCalculateSpace(QStringList& drives, QString& drives_error,
                                               const QDateTime& start_time) {
    Q_EMIT executionProgress("Configuring Disk Cleanup profile...", 5);

    const int PROFILE_ID = 5432;

    // Configure StateFlags registry for comprehensive cleanup
    QString ps_config = QString(
        "$volumeCachesKey = 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VolumeCaches'; "
        "$stateFlags = 'StateFlags%1'; "
        "$cacheFolders = @("
        "  'Active Setup Temp Folders',"
        "  'Downloaded Program Files',"
        "  'Internet Cache Files',"
        "  'Memory Dump Files',"
        "  'Old ChkDsk Files',"
        "  'Previous Installations',"
        "  'Recycle Bin',"
        "  'Setup Log Files',"
        "  'System error memory dump files',"
        "  'System error minidump files',"
        "  'Temporary Files',"
        "  'Temporary Setup Files',"
        "  'Temporary Sync Files',"
        "  'Thumbnail Cache',"
        "  'Update Cleanup',"
        "  'Upgrade Discarded Files',"
        "  'User file versions',"
        "  'Windows Defender',"
        "  'Windows Error Reporting Archive Files',"
        "  'Windows Error Reporting Queue Files',"
        "  'Windows Error Reporting System Archive Files',"
        "  'Windows Error Reporting System Queue Files',"
        "  'Windows Error Reporting Temp Files',"
        "  'Windows ESD installation files',"
        "  'Windows Upgrade Log Files'"
        "); "
        "foreach ($folder in $cacheFolders) { "
        "  try { "
        "    $path = Join-Path $volumeCachesKey $folder; "
        "    if (Test-Path $path) { "
        "      Set-ItemProperty -Path $path -Name $stateFlags -Value 2 -Type DWord -ErrorAction SilentlyContinue; "
        "    } "
        "  } catch {} "
        "}"
    ).arg(PROFILE_ID);

    ProcessResult config_result = runPowerShell(ps_config, sak::kTimeoutArchiveMs);
    if (!config_result.succeeded()) {
        ExecutionResult result;
        result.success = false;
        result.message = "Failed to configure Disk Cleanup";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.log = config_result.std_err.isEmpty() ? "Disk Cleanup configuration failed" : config_result.std_err.trimmed();
        finishWithResult(result, ActionStatus::Failed);
        return false;
    }

    Q_EMIT executionProgress("Running comprehensive Disk Cleanup...", 15);

    // Get all drives for cleanup
    ProcessResult drives_proc = runPowerShell(
        "Get-Volume | Where-Object {$_.DriveLetter -and $_.FileSystem -eq 'NTFS'} | Select-Object -ExpandProperty DriveLetter",
        120000);
    drives = drives_proc.std_out.split('\n', Qt::SkipEmptyParts);
    drives_error = drives_proc.std_err;
    return true;
}

void DiskCleanupAction::executeCleanup(const QStringList& drives, const QString& sagerun_arg,
                                        int& drives_processed, qint64& total_freed) {
    for (const QString& drive : drives) {
        QString drive_letter = drive.trimmed();
        if (drive_letter.isEmpty()) continue;

        if (isCancelled()) return;

        int progress = 15 + ((drives_processed * 70) / qMax(1, static_cast<int>(drives.count())));
        Q_EMIT executionProgress(QString("Cleaning drive %1:...").arg(drive_letter), progress);

        // Get free space before
        ProcessResult space_before = runPowerShell(
            QString("(Get-Volume -DriveLetter %1).SizeRemaining").arg(drive_letter),
            5000);
        bool ok_before = false;
        qint64 free_before = space_before.std_out.trimmed().toLongLong(&ok_before);

        // Run cleanup on this drive
        ProcessResult cleanmgr = runProcess("cleanmgr.exe", QStringList() << "/d" << drive_letter << sagerun_arg, sak::kTimeoutArchiveMs);
        if (!cleanmgr.succeeded()) {
            Q_EMIT executionProgress(QString("Cleanup warning on %1:").arg(drive_letter), progress);
        }

        // Get free space after
        ProcessResult space_after = runPowerShell(
            QString("(Get-Volume -DriveLetter %1).SizeRemaining").arg(drive_letter),
            5000);
        bool ok_after = false;
        qint64 free_after = space_after.std_out.trimmed().toLongLong(&ok_after);

        if (ok_before && ok_after) {
            total_freed += (free_after - free_before);
        }
        drives_processed++;
    }

    Q_EMIT executionProgress("Cleanup complete", 100);
}

void DiskCleanupAction::executeBuildReport(int drives_processed, qint64 total_freed,
                                            const QString& drives_error, const QDateTime& start_time) {
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = drives_processed;
    result.bytes_processed = total_freed;

    if (drives_processed > 0) {
        result.success = true;
        double mb_freed = total_freed / sak::kBytesPerMBf;
        double gb_freed = total_freed / sak::kBytesPerGBf;

        if (gb_freed >= 1.0) {
            result.message = QString("Cleaned %1 drive(s), freed %2 GB")
                .arg(drives_processed).arg(gb_freed, 0, 'f', 2);
        } else {
            result.message = QString("Cleaned %1 drive(s), freed %2 MB")
                .arg(drives_processed).arg(mb_freed, 0, 'f', 1);
        }

        result.log = QString("Completed in %1 seconds\nProfile: Comprehensive Windows cleanup\nDrives processed: %2")
            .arg(duration_ms / 1000).arg(drives_processed);
        if (!drives_error.trimmed().isEmpty()) {
            result.log += QString("\nDrive enumeration errors:\n%1").arg(drives_error.trimmed());
        }
    } else {
        result.success = false;
        result.message = "No drives were cleaned";
        result.log = "Failed to find any NTFS drives to clean";
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

void DiskCleanupAction::scanWindowsTemp() {
    const QString system_root = qEnvironmentVariable("SystemRoot", "C:\\Windows");
    QString path = system_root + "\\Temp";
    QDir dir(path);
    
    if (!dir.exists()) {
        return;
    }

    CleanupTarget target;
    target.path = path;
    target.description = "Windows Temporary Files";
    target.size = calculateDirectorySize(path, target.file_count);
    target.safe_to_delete = true;

    if (target.size > 0) {
        m_targets.push_back(target);
        m_total_bytes += target.size;
        m_total_files += target.file_count;
    }
}

void DiskCleanupAction::scanUserTemp() {
    QString path = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir dir(path);
    
    if (!dir.exists()) {
        return;
    }

    CleanupTarget target;
    target.path = path;
    target.description = "User Temporary Files";
    target.size = calculateDirectorySize(path, target.file_count);
    target.safe_to_delete = true;

    if (target.size > 0) {
        m_targets.push_back(target);
        m_total_bytes += target.size;
        m_total_files += target.file_count;
    }
}

void DiskCleanupAction::scanBrowserCaches() {
    QStringList cache_paths;
    
    // Chrome
    QString chrome_cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) 
                          + "/Google/Chrome/User Data/Default/Cache";
    cache_paths.append(chrome_cache);

    // Firefox
    QString firefox_cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) 
                           + "/Mozilla/Firefox/Profiles";
    cache_paths.append(firefox_cache);

    // Edge
    QString edge_cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) 
                        + "/Microsoft/Edge/User Data/Default/Cache";
    cache_paths.append(edge_cache);

    for (const QString& cache_path : cache_paths) {
        QDir dir(cache_path);
        if (!dir.exists()) {
            continue;
        }

        CleanupTarget target;
        target.path = cache_path;
        target.description = QString("Browser Cache: %1").arg(QFileInfo(cache_path).fileName());
        target.size = calculateDirectorySize(cache_path, target.file_count);
        target.safe_to_delete = true;

        if (target.size > 0) {
            m_targets.push_back(target);
            m_total_bytes += target.size;
            m_total_files += target.file_count;
        }
    }
}

void DiskCleanupAction::scanRecycleBin() {
#ifdef _WIN32
    // Scan Recycle Bin on all available drives
    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo& drive_info : drives) {
        const QString recycle_path = drive_info.absoluteFilePath() + "$Recycle.Bin";
        QDir dir(recycle_path);

        if (!dir.exists()) {
            continue;
        }

        CleanupTarget target;
        target.path = recycle_path;
        target.description = QString("Recycle Bin (%1)").arg(drive_info.absoluteFilePath());
        target.size = calculateDirectorySize(recycle_path, target.file_count);
        target.safe_to_delete = true;

        if (target.size > 0) {
            m_targets.push_back(target);
            m_total_bytes += target.size;
            m_total_files += target.file_count;
        }
    }
#endif
}

void DiskCleanupAction::scanWindowsUpdate() {
    const QString system_root = qEnvironmentVariable("SystemRoot", "C:\\Windows");
    QString path = system_root + "\\SoftwareDistribution\\Download";
    QDir dir(path);
    
    if (!dir.exists()) {
        return;
    }

    CleanupTarget target;
    target.path = path;
    target.description = "Windows Update Downloads";
    target.size = calculateDirectorySize(path, target.file_count);
    target.safe_to_delete = true;

    if (target.size > 0) {
        m_targets.push_back(target);
        m_total_bytes += target.size;
        m_total_files += target.file_count;
    }
}

void DiskCleanupAction::scanThumbnailCache() {
    QString path = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/IconCache.db";
    QFileInfo file_info(path);
    
    if (file_info.exists()) {
        CleanupTarget target;
        target.path = path;
        target.description = "Thumbnail Cache";
        target.size = file_info.size();
        target.file_count = 1;
        target.safe_to_delete = true;

        m_targets.push_back(target);
        m_total_bytes += target.size;
        m_total_files += target.file_count;
    }
}

qint64 DiskCleanupAction::calculateDirectorySize(const QString& path, int& file_count) {
    file_count = 0;
    auto result = path_utils::getDirectorySizeAndCount(path.toStdWString());
    if (!result) {
        return 0;
    }
    file_count = static_cast<int>(result->file_count);
    return static_cast<qint64>(result->total_bytes);
}

qint64 DiskCleanupAction::deleteDirectoryContents(const QString& path, int& deleted_count) {
    qint64 total_deleted = 0;
    deleted_count = 0;

    QDir dir(path);
    if (!dir.exists()) {
        return 0;
    }

    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    
    for (const QFileInfo& entry : entries) {
        if (isCancelled()) {
            break;
        }

        if (entry.isDir()) {
            int subdir_deleted = 0;
            total_deleted += deleteDirectoryContents(entry.absoluteFilePath(), subdir_deleted);
            deleted_count += subdir_deleted;
            
            QDir subdir(entry.absoluteFilePath());
            if (subdir.isEmpty()) {
                subdir.removeRecursively();
            }
        } else {
            qint64 file_size = entry.size();
            if (QFile::remove(entry.absoluteFilePath())) {
                total_deleted += file_size;
                deleted_count++;
            }
        }
    }

    return total_deleted;
}

bool DiskCleanupAction::isSafeToDelete(const QString& path) const {
    // Blacklist critical system paths using environment variables
    const QString system_root = qEnvironmentVariable("SystemRoot", "C:\\Windows");
    const QString system_drive = qEnvironmentVariable("SystemDrive", "C:");
    const QStringList dangerous_paths = {
        system_root + "\\System32",
        system_root + "\\SysWOW64",
        system_drive + "\\Program Files",
        system_drive + "\\Program Files (x86)",
        system_drive + "\\Users\\Public",
        system_drive + "\\ProgramData"
    };

    for (const QString& dangerous : dangerous_paths) {
        if (path.startsWith(dangerous, Qt::CaseInsensitive)) {
            return false;
        }
    }

    return true;
}

} // namespace sak
