// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/disk_cleanup_action.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QStandardPaths>
#include <QDateTime>
#include <QProcess>

namespace sak {

DiskCleanupAction::DiskCleanupAction()
    : QuickAction(nullptr) {
}

void DiskCleanupAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to clean temporary files";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void DiskCleanupAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Configuring Disk Cleanup profile...", 5);
    
    // Enterprise approach: Use /sageset to configure comprehensive cleanup profile
    // Profile 5432 is arbitrary but consistent for this application
    const int PROFILE_ID = 5432;
    QString profile_arg = QString("/sageset:%1").arg(PROFILE_ID);
    QString sagerun_arg = QString("/sagerun:%1").arg(PROFILE_ID);
    
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
    
    QProcess::execute("powershell.exe", QStringList() << "-ExecutionPolicy" << "Bypass" << "-Command" << ps_config);
    
    Q_EMIT executionProgress("Running comprehensive Disk Cleanup...", 15);
    
    // Get all drives for cleanup
    QProcess drives_proc;
    drives_proc.start("powershell.exe", QStringList() << "-Command" 
        << "Get-Volume | Where-Object {$_.DriveLetter -and $_.FileSystem -eq 'NTFS'} | Select-Object -ExpandProperty DriveLetter");
    drives_proc.waitForFinished(5000);
    QString drives_output = drives_proc.readAllStandardOutput();
    QStringList drives = drives_output.split('\n', Qt::SkipEmptyParts);
    
    int drives_processed = 0;
    qint64 total_freed = 0;
    
    for (const QString& drive : drives) {
        QString drive_letter = drive.trimmed();
        if (drive_letter.isEmpty()) continue;
        
        if (isCancelled()) {
            ExecutionResult result;
            result.success = false;
            result.message = "Cleanup cancelled by user";
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            setStatus(ActionStatus::Cancelled);
            Q_EMIT executionComplete(result);
            return;
        }
        
        int progress = 15 + ((drives_processed * 70) / drives.count());
        Q_EMIT executionProgress(QString("Cleaning drive %1:...").arg(drive_letter), progress);
        
        // Get free space before
        QProcess space_before;
        space_before.start("powershell.exe", QStringList() << "-Command" 
            << QString("(Get-Volume -DriveLetter %1).SizeRemaining").arg(drive_letter));
        space_before.waitForFinished(5000);
        qint64 free_before = space_before.readAllStandardOutput().trimmed().toLongLong();
        
        // Run cleanup on this drive
        QProcess cleanmgr;
        cleanmgr.start("cleanmgr.exe", QStringList() << "/d" << drive_letter << sagerun_arg);
        cleanmgr.waitForFinished(300000); // 5 minute timeout per drive
        
        // Get free space after
        QProcess space_after;
        space_after.start("powershell.exe", QStringList() << "-Command" 
            << QString("(Get-Volume -DriveLetter %1).SizeRemaining").arg(drive_letter));
        space_after.waitForFinished(5000);
        qint64 free_after = space_after.readAllStandardOutput().trimmed().toLongLong();
        
        total_freed += (free_after - free_before);
        drives_processed++;
    }
    
    Q_EMIT executionProgress("Cleanup complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = drives_processed;
    result.bytes_processed = total_freed;
    
    if (drives_processed > 0) {
        result.success = true;
        double mb_freed = total_freed / (1024.0 * 1024.0);
        double gb_freed = total_freed / (1024.0 * 1024.0 * 1024.0);
        
        if (gb_freed >= 1.0) {
            result.message = QString("Cleaned %1 drive(s), freed %2 GB")
                .arg(drives_processed).arg(gb_freed, 0, 'f', 2);
        } else {
            result.message = QString("Cleaned %1 drive(s), freed %2 MB")
                .arg(drives_processed).arg(mb_freed, 0, 'f', 1);
        }
        
        result.log = QString("Completed in %1 seconds\nProfile: Comprehensive Windows cleanup\nDrives processed: %2")
            .arg(duration_ms / 1000).arg(drives_processed);
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "No drives were cleaned";
        result.log = "Failed to find any NTFS drives to clean";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

void DiskCleanupAction::scanWindowsTemp() {
    QString path = "C:\\Windows\\Temp";
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
    QString recycle_path = "C:\\$Recycle.Bin";
    QDir dir(recycle_path);
    
    if (!dir.exists()) {
        return;
    }

    CleanupTarget target;
    target.path = recycle_path;
    target.description = "Recycle Bin";
    target.size = calculateDirectorySize(recycle_path, target.file_count);
    target.safe_to_delete = true;

    if (target.size > 0) {
        m_targets.push_back(target);
        m_total_bytes += target.size;
        m_total_files += target.file_count;
    }
#endif
}

void DiskCleanupAction::scanWindowsUpdate() {
    QString path = "C:\\Windows\\SoftwareDistribution\\Download";
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
    qint64 total_size = 0;
    file_count = 0;

    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (isCancelled()) {
            break;
        }

        it.next();
        QFileInfo file_info = it.fileInfo();
        total_size += file_info.size();
        file_count++;
    }

    return total_size;
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
    // Blacklist critical system paths
    QStringList dangerous_paths = {
        "C:\\Windows\\System32",
        "C:\\Windows\\SysWOW64",
        "C:\\Program Files",
        "C:\\Program Files (x86)",
        "C:\\Users\\Public",
        "C:\\ProgramData"
    };

    for (const QString& dangerous : dangerous_paths) {
        if (path.startsWith(dangerous, Qt::CaseInsensitive)) {
            return false;
        }
    }

    return true;
}

} // namespace sak
