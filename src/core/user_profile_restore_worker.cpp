#include "sak/user_profile_restore_worker.h"
#include "sak/smart_file_filter.h"
#include "sak/permission_manager.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace sak {

UserProfileRestoreWorker::UserProfileRestoreWorker(QObject* parent)
    : QThread(parent)
    , m_fileFilter(new SmartFileFilter())
    , m_permissionManager(new PermissionManager())
{
}

UserProfileRestoreWorker::~UserProfileRestoreWorker() {
    if (isRunning()) {
        cancel();
        wait(5000);
    }
}

void UserProfileRestoreWorker::startRestore(const QString& backupPath,
                                            const BackupManifest& manifest,
                                            const QVector<UserMapping>& mappings,
                                            ConflictResolution conflictMode,
                                            PermissionMode permMode,
                                            bool verify) {
    if (m_running) {
        Q_EMIT logMessage(tr("Restore already in progress"), true);
        return;
    }
    
    QMutexLocker locker(&m_mutex);
    
    m_backupPath = backupPath;
    m_manifest = manifest;
    m_mappings = mappings;
    m_conflictMode = conflictMode;
    m_permissionMode = permMode;
    m_verify = verify;
    
    m_cancelled = false;
    m_bytesRestored = 0;
    m_filesRestored = 0;
    m_filesSkipped = 0;
    m_filesErrored = 0;
    
    start();
}

void UserProfileRestoreWorker::cancel() {
    m_cancelled = true;
    Q_EMIT logMessage(tr("Canceling restore..."), false);
}

void UserProfileRestoreWorker::run() {
    m_running = true;
    
    Q_EMIT logMessage(tr("=== Restore Started ==="), false);
    Q_EMIT logMessage(tr("Backup: %1").arg(m_backupPath), false);
    Q_EMIT logMessage(tr("Users to restore: %1").arg(m_mappings.size()), false);
    
    // Validate backup
    if (!validateBackup()) {
        Q_EMIT restoreComplete(false, tr("Invalid backup"));
        m_running = false;
        return;
    }
    
    // Calculate total size
    Q_EMIT logMessage(tr("Calculating total size..."), false);
    m_totalBytesToRestore = calculateTotalSize();
    
    // Restore each mapped user
    int userIndex = 0;
    for (const auto& mapping : m_mappings) {
        if (m_cancelled) {
            Q_EMIT logMessage(tr("Restore cancelled by user"), true);
            Q_EMIT restoreComplete(false, tr("Restore cancelled"));
            m_running = false;
            return;
        }
        
        if (!mapping.selected) continue;
        
        Q_EMIT statusUpdate(mapping.source_username, tr("Starting restore..."));
        Q_EMIT logMessage(tr("=== Restoring user: %1 → %2 ===")
            .arg(mapping.source_username, mapping.destination_username.isEmpty() ? tr("(New)") : mapping.destination_username), false);
        
        if (!restoreUser(mapping)) {
            Q_EMIT logMessage(tr("Failed to restore user: %1").arg(mapping.source_username), true);
        }
        
        userIndex++;
        Q_EMIT overallProgress(userIndex, m_mappings.size(), m_bytesRestored, m_totalBytesToRestore);
    }
    
    // Complete
    QString summary = tr("Restore complete!\nFiles restored: %1\nFiles skipped: %2\nErrors: %3\nTotal size: %4 MB")
        .arg(m_filesRestored)
        .arg(m_filesSkipped)
        .arg(m_filesErrored)
        .arg(m_bytesRestored / (1024.0 * 1024.0), 0, 'f', 1);
    
    Q_EMIT logMessage(tr("=== Restore Complete ==="), false);
    Q_EMIT logMessage(summary, false);
    Q_EMIT restoreComplete(true, summary);
    
    m_running = false;
}

bool UserProfileRestoreWorker::restoreUser(const UserMapping& mapping) {
    // Find source user data in manifest
    const BackupUserData* sourceUser = nullptr;
    for (const auto& user : m_manifest.users) {
        if (user.username == mapping.source_username) {
            sourceUser = &user;
            break;
        }
    }
    
    if (!sourceUser) {
        Q_EMIT logMessage(tr("Source user not found in manifest: %1").arg(mapping.source_username), true);
        return false;
    }
    
    QString sourcePath = m_backupPath + "/" + mapping.source_username;
    QString destProfilePath;
    QString systemDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));
    if (systemDrive.isEmpty()) systemDrive = "C:";
    
    // Determine destination path based on merge mode
    switch (mapping.mode) {
        case MergeMode::CreateNewUser:
            // For new user, create profile path
            destProfilePath = systemDrive + "/Users/" + mapping.source_username;
            if (!QDir().mkpath(destProfilePath)) {
                Q_EMIT logMessage(tr("Failed to create profile directory: %1").arg(destProfilePath), true);
                return false;
            }
            Q_EMIT logMessage(tr("Created new profile: %1").arg(destProfilePath), false);
            break;
            
        case MergeMode::ReplaceDestination:
        case MergeMode::MergeIntoDestination:
            // Use existing destination user's profile
            if (mapping.destination_username.isEmpty()) {
                Q_EMIT logMessage(tr("Destination username not specified"), true);
                return false;
            }
            
            // Find destination profile path (would need WindowsUserScanner here)
            destProfilePath = systemDrive + "/Users/" + mapping.destination_username;
            
            if (!QDir(destProfilePath).exists()) {
                Q_EMIT logMessage(tr("Destination profile does not exist: %1").arg(destProfilePath), true);
                return false;
            }
            break;
    }
    
    // Restore each folder
    for (const auto& folder : sourceUser->backed_up_folders) {
        if (m_cancelled) return false;
        
        Q_EMIT statusUpdate(mapping.source_username, tr("Restoring: %1").arg(folder.display_name));
        
        QString folderSourcePath = sourcePath + "/" + folder.relative_path;
        QString folderDestPath = destProfilePath + "/" + folder.relative_path;
        
        if (!restoreFolder(folder, folderSourcePath, folderDestPath)) {
            Q_EMIT logMessage(tr("Warning: Failed to restore folder: %1").arg(folder.display_name), true);
            // Continue with other folders
        }
    }
    
    return true;
}

bool UserProfileRestoreWorker::restoreFolder(const FolderSelection& folder,
                                             const QString& sourcePath,
                                             const QString& destPath) {
    QFileInfo sourceInfo(sourcePath);
    
    if (!sourceInfo.exists()) {
        Q_EMIT logMessage(tr("Source folder does not exist: %1").arg(sourcePath), true);
        return false;
    }
    
    // Create destination directory
    QDir destDir(destPath);
    if (!destDir.exists()) {
        if (!QDir().mkpath(destPath)) {
            Q_EMIT logMessage(tr("Failed to create directory: %1").arg(destPath), true);
            return false;
        }
    }
    
    // Recursively copy directory contents
    return copyDirectory(sourcePath, destPath, folder);
}

bool UserProfileRestoreWorker::copyDirectory(const QString& sourceDir,
                                             const QString& destDir,
                                             const FolderSelection& folderConfig) {
    QDir dir(sourceDir);
    if (!dir.exists()) return false;
    
    // Create destination directory
    QDir().mkpath(destDir);
    
    // Iterate through all entries
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    
    for (const QFileInfo& entry : entries) {
        if (m_cancelled) return false;
        
        QString sourceItem = entry.absoluteFilePath();
        QString destItem = destDir + "/" + entry.fileName();
        
        if (entry.isDir()) {
            // Recursively copy subdirectory
            if (!copyDirectory(sourceItem, destItem, folderConfig)) {
                Q_EMIT logMessage(tr("Warning: Failed to copy directory: %1").arg(sourceItem), true);
            }
        } else if (entry.isFile()) {
            // Copy file with conflict resolution
            if (!copyFileWithConflictResolution(sourceItem, destItem, entry.size())) {
                // Error already logged
            }
        }
    }
    
    return true;
}

bool UserProfileRestoreWorker::copyFileWithConflictResolution(const QString& source,
                                                              const QString& dest,
                                                              qint64 size) {
    QFileInfo destInfo(dest);
    QString finalDestPath = dest;
    
    // Check if destination file exists
    if (destInfo.exists()) {
        // Apply conflict resolution strategy
        switch (m_conflictMode) {
            case ConflictResolution::SkipDuplicate:
                Q_EMIT logMessage(tr("Skipping existing file: %1").arg(destInfo.fileName()), false);
                m_filesSkipped++;
                return true;
                
            case ConflictResolution::RenameWithSuffix: {
                // Add suffix to avoid conflict
                QFileInfo sourceInfo(source);
                QString baseName = destInfo.completeBaseName();
                QString extension = destInfo.suffix();
                QString dirPath = destInfo.absolutePath();
                
                int counter = 1;
                do {
                    finalDestPath = QString("%1/%2_backup%3%4")
                        .arg(dirPath, baseName, QString::number(counter++),
                             extension.isEmpty() ? "" : "." + extension);
                } while (QFileInfo::exists(finalDestPath) && counter < 1000);
                
                Q_EMIT logMessage(tr("Renaming to avoid conflict: %1").arg(QFileInfo(finalDestPath).fileName()), false);
                break;
            }
            
            case ConflictResolution::KeepNewer: {
                QFileInfo sourceInfo(source);
                if (destInfo.lastModified() >= sourceInfo.lastModified()) {
                    Q_EMIT logMessage(tr("Keeping newer existing file: %1").arg(destInfo.fileName()), false);
                    m_filesSkipped++;
                    return true;
                }
                // Otherwise overwrite
                Q_EMIT logMessage(tr("Replacing with newer file: %1").arg(destInfo.fileName()), false);
                QFile::remove(dest);
                break;
            }
            
            case ConflictResolution::KeepLarger: {
                if (destInfo.size() >= size) {
                    Q_EMIT logMessage(tr("Keeping larger existing file: %1").arg(destInfo.fileName()), false);
                    m_filesSkipped++;
                    return true;
                }
                // Otherwise overwrite
                Q_EMIT logMessage(tr("Replacing with larger file: %1").arg(destInfo.fileName()), false);
                QFile::remove(dest);
                break;
            }
            
            case ConflictResolution::PromptUser:
                // Auto-rename (interactive prompts can't run in background worker)
                finalDestPath = resolveConflict(source, dest);
                Q_EMIT logMessage(tr("File exists, auto-renamed: %1 → %2")
                    .arg(destInfo.fileName(), QFileInfo(finalDestPath).fileName()), false);
                break;
        }
    }
    
    // Ensure destination directory exists
    QFileInfo finalDestInfo(finalDestPath);
    QDir().mkpath(finalDestInfo.absolutePath());
    
    // Copy the file
    if (!QFile::copy(source, finalDestPath)) {
        Q_EMIT logMessage(tr("Error copying file: %1").arg(source), true);
        m_filesErrored++;
        return false;
    }
    
    // Apply permissions based on mode
    if (!applyPermissions(finalDestPath, "")) {
        Q_EMIT logMessage(tr("Warning: Failed to adjust permissions: %1").arg(finalDestPath), true);
        // Continue anyway
    }
    
    // Verify file if requested
    if (m_verify) {
        if (!verifyFile(finalDestPath)) {
            Q_EMIT logMessage(tr("Warning: File verification failed: %1").arg(finalDestPath), true);
        }
    }
    
    // Update progress
    m_filesRestored++;
    updateProgress(size);
    
    return true;
}

bool UserProfileRestoreWorker::applyPermissions(const QString& filePath,
                                                const QString& destinationUser) {
    switch (m_permissionMode) {
        case PermissionMode::StripAll:
            // Remove all ACLs, inherit from parent
            return m_permissionManager->stripPermissions(filePath);
            
        case PermissionMode::PreserveOriginal:
            // Keep existing permissions from backup (already set by copy)
            return true;
            
        case PermissionMode::AssignToDestination:
            // Assign ownership to destination user
            if (destinationUser.isEmpty()) {
                // If no specific user, strip permissions
                return m_permissionManager->stripPermissions(filePath);
            }
            // Would need to implement setOwner() in PermissionManager
            // For now, strip permissions as fallback
            return m_permissionManager->stripPermissions(filePath);
            
        case PermissionMode::Hybrid:
            // Try to preserve, fall back to strip on error
            if (!m_permissionManager->stripPermissions(filePath)) {
                return m_permissionManager->stripPermissions(filePath);
            }
            return true;
    }
    
    return true;
}

bool UserProfileRestoreWorker::validateBackup() {
    QFileInfo manifestFile(m_backupPath + "/manifest.json");
    if (!manifestFile.exists()) {
        Q_EMIT logMessage(tr("Manifest file not found"), true);
        return false;
    }
    
    Q_EMIT logMessage(tr("Backup validation passed"), false);
    return true;
}

bool UserProfileRestoreWorker::createRestoreStructure() {
    Q_EMIT logMessage(tr("Creating restore directory structure..."), false);
    
    // Create destination directories for each mapping
    for (const auto& mapping : m_mappings) {
        if (!mapping.selected) continue;
        
        // Get destination profile path from username
        QString destPath = WindowsUserScanner::getProfilePath(mapping.destination_username);
        if (destPath.isEmpty()) {
            Q_EMIT logMessage(tr("Failed to resolve profile path for user: %1").arg(mapping.destination_username), true);
            return false;
        }
        
        QDir destDir(destPath);
        
        if (!destDir.exists()) {
            if (!destDir.mkpath(".")) {
                Q_EMIT logMessage(tr("Failed to create directory: %1").arg(destPath), true);
                return false;
            }
            Q_EMIT logMessage(tr("Created directory: %1").arg(destPath), false);
        }
        
        // Create standard user profile subdirectories
        QStringList standardFolders = {
            "Documents", "Desktop", "Pictures", "Videos", 
            "Music", "Downloads", "AppData", "AppData/Local", 
            "AppData/Roaming", "Favorites"
        };
        
        for (const QString& folder : standardFolders) {
            QString folderPath = destDir.filePath(folder);
            QDir folderDir(folderPath);
            
            if (!folderDir.exists()) {
                if (!folderDir.mkpath(".")) {
                    Q_EMIT logMessage(tr("Failed to create subdirectory: %1").arg(folderPath), true);
                    // Continue anyway - not all folders may be needed
                }
            }
        }
    }
    
    Q_EMIT logMessage(tr("Restore directory structure created"), false);
    return true;
}

qint64 UserProfileRestoreWorker::calculateTotalSize() {
    qint64 totalSize = 0;
    m_totalFilesToRestore = 0;
    
    for (const auto& mapping : m_mappings) {
        if (!mapping.selected) continue;
        
        // Find user data in manifest
        for (const auto& user : m_manifest.users) {
            if (user.username == mapping.source_username) {
                for (const auto& folder : user.backed_up_folders) {
                    totalSize += folder.size_bytes;
                    m_totalFilesToRestore += folder.file_count;
                }
                break;
            }
        }
    }
    
    return totalSize;
}

void UserProfileRestoreWorker::updateProgress(qint64 bytesAdded) {
    m_bytesRestored += bytesAdded;
    
    static int lastFileCount = 0;
    static qint64 lastByteCount = 0;
    
    if (m_filesRestored - lastFileCount >= 100 ||
        m_bytesRestored - lastByteCount >= 100 * 1024 * 1024) {
        
        Q_EMIT fileProgress(m_filesRestored, m_totalFilesToRestore);
        
        lastFileCount = m_filesRestored;
        lastByteCount = m_bytesRestored;
    }
}

bool UserProfileRestoreWorker::verifyFile(const QString& filePath) {
    QFileInfo fileInfo(filePath);
    
    if (!fileInfo.exists()) {
        Q_EMIT logMessage(tr("Verification failed - file missing: %1").arg(filePath), true);
        return false;
    }
    
    if (!fileInfo.isReadable()) {
        Q_EMIT logMessage(tr("Verification failed - file not readable: %1").arg(filePath), true);
        return false;
    }
    
    // Basic verification - file exists and is readable
    // Could add checksum verification if checksums were stored in manifest
    return true;
}

QString UserProfileRestoreWorker::resolveConflict(const QString& sourcePath,
                                                  const QString& destPath) {
    Q_UNUSED(sourcePath);
    
    // Generate unique filename by adding suffix
    QFileInfo destInfo(destPath);
    QString baseName = destInfo.completeBaseName();
    QString extension = destInfo.suffix();
    QString dirPath = destInfo.absolutePath();
    
    int counter = 1;
    QString newPath;
    do {
        newPath = QString("%1/%2_restored%3%4")
            .arg(dirPath, baseName, QString::number(counter++),
                 extension.isEmpty() ? "" : "." + extension);
    } while (QFileInfo::exists(newPath) && counter < 1000);
    
    return newPath;
}

} // namespace sak
