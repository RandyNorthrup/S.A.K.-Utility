// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/disk_cleanup_action.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QStandardPaths>
#include <QDateTime>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace sak {

DiskCleanupAction::DiskCleanupAction()
    : QuickAction(nullptr) {
}

void DiskCleanupAction::scan() {
    setStatus(ActionStatus::Scanning);
    m_targets.clear();
    m_total_bytes = 0;
    m_total_files = 0;

    // Scan each target
    scanWindowsTemp();
    scanUserTemp();
    scanBrowserCaches();
    scanRecycleBin();
    scanWindowsUpdate();
    scanThumbnailCache();

    // Build summary
    QString summary;
    if (m_total_bytes > 0) {
        summary = QString("Found %1 items in %2 locations")
                     .arg(m_total_files)
                     .arg(m_targets.size());
    } else {
        summary = "No cleanup needed - system is clean";
    }

    // Estimate duration (1MB per second)
    qint64 estimated_ms = (m_total_bytes / (1024 * 1024)) * 1000;
    if (estimated_ms < 1000) {
        estimated_ms = 1000;
    }

    ScanResult result;
    result.applicable = m_total_bytes > 0;
    result.summary = summary;
    result.bytes_affected = m_total_bytes;
    result.files_count = m_total_files;
    result.estimated_duration_ms = estimated_ms;
    result.warning = "This will permanently delete temporary files. System may temporarily slow down while caches rebuild.";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void DiskCleanupAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    
    qint64 total_deleted = 0;
    int total_files_deleted = 0;
    int progress = 0;
    const int total_targets = static_cast<int>(m_targets.size());

    QDateTime start_time = QDateTime::currentDateTime();

    for (const auto& target : m_targets) {
        if (isCancelled()) {
            setStatus(ActionStatus::Cancelled);
            ExecutionResult result;
            result.success = false;
            result.message = "Cleanup cancelled by user";
            result.bytes_processed = total_deleted;
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            Q_EMIT executionComplete(result);
            return;
        }

        QString prog_msg = QString("Cleaning %1...").arg(target.description);
        Q_EMIT executionProgress(prog_msg, (progress * 100) / total_targets);
        progress++;

        if (!target.safe_to_delete) {
            continue;
        }

        int deleted_count = 0;
        qint64 deleted_bytes = deleteDirectoryContents(target.path, deleted_count);
        total_deleted += deleted_bytes;
        total_files_deleted += deleted_count;
    }

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.success = true;
    result.message = QString("Cleaned up %1 files").arg(total_files_deleted);
    result.bytes_processed = total_deleted;
    result.duration_ms = duration_ms;
    result.files_processed = total_files_deleted;
    result.log = QString("Deleted %1 items from %2 locations in %3ms")
                    .arg(total_files_deleted)
                    .arg(m_targets.size())
                    .arg(duration_ms);

    setExecutionResult(result);
    setStatus(ActionStatus::Success);
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
