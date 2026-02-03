// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/photo_management_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>

namespace sak {

PhotoManagementBackupAction::PhotoManagementBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

void PhotoManagementBackupAction::scanLightroomCatalogs() {
    for (const UserProfile& user : m_user_profiles) {
        QStringList search_paths = {
            user.profile_path + "/Pictures/Lightroom",
            user.profile_path + "/Documents/Lightroom"
        };
        
        for (const QString& path : search_paths) {
            QDir dir(path);
            if (!dir.exists()) continue;
            
            QDirIterator it(path, QStringList() << "*.lrcat", QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                PhotoSoftwareData data;
                data.software_name = "Lightroom";
                data.data_type = "Catalog";
                data.path = it.filePath();
                data.size = it.fileInfo().size();
                
                m_photo_data.append(data);
                m_total_size += data.size;
            }
        }
    }
}

void PhotoManagementBackupAction::scanPhotoshopSettings() {
    for (const UserProfile& user : m_user_profiles) {
        QString ps_path = user.profile_path + "/AppData/Roaming/Adobe/Adobe Photoshop";
        QDir dir(ps_path);
        
        if (dir.exists()) {
            QDirIterator it(ps_path, QDir::Dirs | QDir::NoDotAndDotDot);
            while (it.hasNext()) {
                it.next();
                QString presets = it.filePath() + "/Presets";
                
                if (QDir(presets).exists()) {
                    PhotoSoftwareData data;
                    data.software_name = "Photoshop";
                    data.data_type = "Presets";
                    data.path = presets;
                    
                    qint64 size = 0;
                    QDirIterator files(presets, QDir::Files, QDirIterator::Subdirectories);
                    while (files.hasNext()) {
                        files.next();
                        size += files.fileInfo().size();
                    }
                    
                    data.size = size;
                    m_photo_data.append(data);
                    m_total_size += size;
                }
            }
        }
    }
}

void PhotoManagementBackupAction::scanCaptureOne() {
    for (const UserProfile& user : m_user_profiles) {
        QString c1_path = user.profile_path + "/Pictures/Capture One";
        QDir dir(c1_path);
        
        if (dir.exists()) {
            QDirIterator it(c1_path, QStringList() << "*.cosessiondb", 
                          QDir::Files, QDirIterator::Subdirectories);
            
            while (it.hasNext()) {
                it.next();
                PhotoSoftwareData data;
                data.software_name = "Capture One";
                data.data_type = "Catalog";
                data.path = it.filePath();
                data.size = it.fileInfo().size();
                
                m_photo_data.append(data);
                m_total_size += data.size;
            }
        }
    }
}

void PhotoManagementBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    
    WindowsUserScanner scanner;
    m_user_profiles = scanner.scanUsers();
    
    m_photo_data.clear();
    m_total_size = 0;
    
    scanLightroomCatalogs();
    scanPhotoshopSettings();
    scanCaptureOne();
    
    ScanResult result;
    result.applicable = (m_photo_data.count() > 0);
    result.bytes_affected = m_total_size;
    result.files_count = m_photo_data.count();
    result.estimated_duration_ms = (m_total_size / (20 * 1024 * 1024)) * 1000;
    
    if (m_photo_data.count() > 0) {
        result.summary = QString("Found %1 photo software item(s) - %2 MB")
            .arg(m_photo_data.count())
            .arg(m_total_size / (1024 * 1024));
    } else {
        result.summary = "No photo management software data found";
    }
    
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void PhotoManagementBackupAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    QDir backup_dir(m_backup_location + "/PhotoSoftware");
    backup_dir.mkpath(".");
    
    int processed = 0;
    qint64 bytes_copied = 0;
    
    for (const PhotoSoftwareData& data : m_photo_data) {
        if (isCancelled()) {
            ExecutionResult result;
            result.success = false;
            result.message = "Photo software backup cancelled";
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            setStatus(ActionStatus::Cancelled);
            Q_EMIT executionComplete(result);
            return;
        }

        QString safe_dir = data.path;
        safe_dir.replace(':', '_');
        safe_dir.replace('\\', '_');
        safe_dir.replace('/', '_');
        QString dest_path = backup_dir.filePath(data.software_name + "/" + data.data_type + "/" + safe_dir);
        QDir().mkpath(dest_path);
        
        QFileInfo src_info(data.path);
        
        if (src_info.isFile()) {
            QString dest_file = dest_path + "/" + src_info.fileName();
            if (QFile::exists(dest_file)) {
                QString base = src_info.completeBaseName();
                QString ext = src_info.suffix();
                int suffix = 1;
                QString candidate;
                do {
                    candidate = dest_path + "/" + QString("%1_%2.%3").arg(base).arg(suffix).arg(ext);
                    suffix++;
                } while (QFile::exists(candidate));
                dest_file = candidate;
            }
            if (QFile::copy(data.path, dest_file)) {
                processed++;
                bytes_copied += data.size;
            }
        } else if (src_info.isDir()) {
            // Copy directory recursively
            QDirIterator it(data.path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                QString rel = QDir(data.path).relativeFilePath(it.filePath());
                QString dest = dest_path + "/" + rel;
                
                QDir().mkpath(QFileInfo(dest).absolutePath());
                if (QFile::copy(it.filePath(), dest)) {
                    bytes_copied += it.fileInfo().size();
                }
            }
            processed++;
        }
        
        Q_EMIT executionProgress(QString("Backing up %1 %2...").arg(data.software_name, data.data_type),
                             (processed * 100) / m_photo_data.count());
    }
    
    ExecutionResult result;
    result.success = processed > 0;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.files_processed = processed;
    result.bytes_processed = bytes_copied;
    result.message = processed > 0
        ? QString("Backed up %1 photo software item(s)").arg(processed)
        : "No photo software data was backed up";
    result.output_path = backup_dir.absolutePath();
    
    setExecutionResult(result);
    setStatus(processed > 0 ? ActionStatus::Success : ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

} // namespace sak
