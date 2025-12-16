// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/rebuild_icon_cache_action.h"
#include <QProcess>
#include <QThread>
#include <QDir>
#include <QStandardPaths>
#include <QDirIterator>
#include <QFile>

namespace sak {

RebuildIconCacheAction::RebuildIconCacheAction(QObject* parent)
    : QuickAction(parent)
{
}

struct CacheFileInfo {
    QString file_name;
    qint64 size_bytes;
    bool exists;
};

// ENTERPRISE-GRADE: Enumerate all cache files (IconCache.db and thumbcache_*.db)
QVector<RebuildIconCacheAction::CacheFileInfo> RebuildIconCacheAction::enumerateCacheFiles() {
    QVector<RebuildIconCacheAction::CacheFileInfo> cache_files;
    
    QString local_app_data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    local_app_data += "/../Local";
    
    QDir cache_dir(QDir::cleanPath(local_app_data));
    
    if (!cache_dir.exists()) {
        return cache_files;
    }
    
    // IconCache.db in Local\Microsoft\Windows\Explorer
    QString explorer_path = cache_dir.absoluteFilePath("Microsoft/Windows/Explorer");
    QDir explorer_dir(explorer_path);
    
    if (explorer_dir.exists()) {
        QStringList icon_filters;
        icon_filters << "IconCache.db" << "iconcache_*.db";
        
        QDirIterator it(explorer_dir.absolutePath(), icon_filters, QDir::Files);
        while (it.hasNext()) {
            it.next();
            CacheFileInfo info;
            info.file_name = it.filePath();
            info.size_bytes = it.fileInfo().size();
            info.exists = true;
            cache_files.append(info);
        }
    }
    
    // Thumbcache files in Local\Microsoft\Windows\Explorer
    QStringList thumb_filters;
    thumb_filters << "thumbcache_*.db";
    
    if (explorer_dir.exists()) {
        QDirIterator thumb_it(explorer_dir.absolutePath(), thumb_filters, QDir::Files);
        while (thumb_it.hasNext()) {
            thumb_it.next();
            CacheFileInfo info;
            info.file_name = thumb_it.filePath();
            info.size_bytes = thumb_it.fileInfo().size();
            info.exists = true;
            cache_files.append(info);
        }
    }
    
    return cache_files;
}

// ENTERPRISE-GRADE: Stop Explorer using Stop-Process cmdlet
bool RebuildIconCacheAction::stopExplorer() {
    Q_EMIT executionProgress("Stopping Windows Explorer (Stop-Process)...", 20);
    
    QString ps_cmd = "Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    proc.waitForFinished(5000);
    
    // Give Explorer time to fully stop
    QThread::msleep(2000);
    
    // Verify Explorer is stopped
    QString check_cmd = "(Get-Process -Name explorer -ErrorAction SilentlyContinue | Measure-Object).Count";
    QProcess check_proc;
    check_proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << check_cmd);
    check_proc.waitForFinished(3000);
    
    int count = check_proc.readAllStandardOutput().trimmed().toInt();
    return count == 0;
}

// ENTERPRISE-GRADE: Delete cache files with verification
int RebuildIconCacheAction::deleteCacheFiles(const QVector<CacheFileInfo>& files) {
    Q_EMIT executionProgress("Deleting icon and thumbnail cache files...", 45);
    
    int deleted_count = 0;
    
    for (const CacheFileInfo& info : files) {
        // Use QFile::remove for reliable deletion
        if (QFile::exists(info.file_name)) {
            if (QFile::remove(info.file_name)) {
                deleted_count++;
            } else {
                // Try PowerShell Remove-Item if QFile fails
                QString ps_cmd = QString("Remove-Item -Path '%1' -Force -ErrorAction SilentlyContinue").arg(info.file_name);
                QProcess::execute("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
                
                if (!QFile::exists(info.file_name)) {
                    deleted_count++;
                }
            }
        }
    }
    
    return deleted_count;
}

// ENTERPRISE-GRADE: Start Explorer with Start-Process
bool RebuildIconCacheAction::startExplorer() {
    Q_EMIT executionProgress("Starting Windows Explorer...", 70);
    
    QString ps_cmd = "Start-Process explorer.exe";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    proc.waitForFinished(5000);
    
    // Give Explorer time to start
    QThread::msleep(2000);
    
    // Verify Explorer is running
    QString check_cmd = "(Get-Process -Name explorer -ErrorAction SilentlyContinue | Measure-Object).Count";
    QProcess check_proc;
    check_proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << check_cmd);
    check_proc.waitForFinished(3000);
    
    int count = check_proc.readAllStandardOutput().trimmed().toInt();
    return count > 0;
}

// ENTERPRISE-GRADE: Refresh Shell icon cache using SHChangeNotify
bool RebuildIconCacheAction::refreshIconCache() {
    Q_EMIT executionProgress("Refreshing Shell icon cache...", 85);
    
    // Use PowerShell to call SHChangeNotify via P/Invoke
    QString ps_cmd = "Add-Type -TypeDefinition @'\n"
                    "using System;\n"
                    "using System.Runtime.InteropServices;\n"
                    "public class Shell32 {\n"
                    "    [DllImport(\"shell32.dll\")]\n"
                    "    public static extern void SHChangeNotify(int eventId, int flags, IntPtr item1, IntPtr item2);\n"
                    "}\n"
                    "'@\n"
                    "[Shell32]::SHChangeNotify(0x8000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)";
    
    QProcess::execute("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    
    return true;
}

void RebuildIconCacheAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to rebuild icon cache";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void RebuildIconCacheAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Enumerating cache files...", 5);
    
    // PHASE 1: Enumerate cache files
    QVector<CacheFileInfo> cache_files = enumerateCacheFiles();
    
    qint64 total_size = 0;
    for (const CacheFileInfo& info : cache_files) {
        total_size += info.size_bytes;
    }
    
    QString report = "╔════════════════════════════════════════════════════════════════╗\n";
    report += "║           ICON & THUMBNAIL CACHE REBUILD REPORT              ║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ Cache Files Found: %1\n").arg(cache_files.size()).leftJustified(67, ' ') + "║\n";
    report += QString("║ Total Cache Size:  %1 KB\n").arg(total_size / 1024).leftJustified(67, ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    
    // List cache files
    if (!cache_files.isEmpty()) {
        report += QString("║ Cache Files:\n").leftJustified(67, ' ') + "║\n";
        for (const CacheFileInfo& info : cache_files) {
            QFileInfo fi(info.file_name);
            QString file_line = QString("║   • %1 (%2 KB)\n")
                                   .arg(fi.fileName())
                                   .arg(info.size_bytes / 1024);
            report += file_line.leftJustified(67, ' ') + "║\n";
        }
        report += "╠════════════════════════════════════════════════════════════════╣\n";
    }
    
    // PHASE 2: Stop Explorer
    bool explorer_stopped = stopExplorer();
    report += QString("║ Explorer Stopped:  %1\n").arg(explorer_stopped ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    
    if (!explorer_stopped) {
        report += QString("║ WARNING: Explorer did not stop cleanly\n").leftJustified(67, ' ') + "║\n";
    }
    
    // PHASE 3: Delete cache files
    int deleted_count = deleteCacheFiles(cache_files);
    report += QString("║ Files Deleted:     %1 / %2\n").arg(deleted_count).arg(cache_files.size()).leftJustified(67, ' ') + "║\n";
    
    // PHASE 5: Start Explorer
    bool explorer_started = startExplorer();
    report += (QString("║ Explorer Started:  %1\n").arg(explorer_started ? "SUCCESS" : "FAILED") + "║\n");
    
    // PHASE 5: Refresh icon cache
    if (explorer_started) {
        refreshIconCache();
        report += "║ Icon Cache:        Refreshed                     ║\n";
    }
    
    report += "╚════════════════════════════════════════════════════════════════╝\n";
    
    Q_EMIT executionProgress("Icon cache rebuild complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = deleted_count;
    result.bytes_processed = total_size;
    
    bool overall_success = explorer_stopped && (deleted_count > 0) && explorer_started;
    
    if (overall_success) {
        result.success = true;
        result.message = QString("Icon cache rebuilt: %1 files deleted (%2 KB freed)")
                            .arg(deleted_count)
                            .arg(total_size / 1024);
        result.log = report;
        result.log += QString("\nCompleted in %1 seconds\n").arg(duration_ms / 1000);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "• Icons will refresh automatically\n";
        result.log += "• Thumbnails will regenerate as needed\n";
        result.log += "• No reboot required\n";
        setStatus(ActionStatus::Success);
    } else if (explorer_started) {
        result.success = true;
        result.message = QString("Icon cache rebuilt with warnings (%1 files)").arg(deleted_count);
        result.log = report;
        result.log += "\nExplorer restarted but some cache files may not have been deleted\n";
        setStatus(ActionStatus::Failed);
    } else {
        result.success = false;
        result.message = "Failed to restart Windows Explorer";
        result.log = report;
        result.log += "\nCritical error: Explorer did not restart - manual intervention required\n";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak
