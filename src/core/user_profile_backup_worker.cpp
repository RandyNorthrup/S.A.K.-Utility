// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_worker.h"

#include "sak/elevation_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStorageInfo>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

UserProfileBackupWorker::UserProfileBackupWorker(QObject* parent)
    : QThread(parent)
    , m_fileFilter(new SmartFileFilter())
    , m_permissionManager(new PermissionManager()) {}

UserProfileBackupWorker::~UserProfileBackupWorker() {
    if (isRunning()) {
        cancel();
        wait(5000);
    }
}

void UserProfileBackupWorker::startBackup(const BackupManifest& manifest,
                                          const QVector<UserProfile>& users,
                                          const QString& destinationPath,
                                          const SmartFilter& smartFilter,
                                          const BackupOptions& options) {
    Q_ASSERT_X(!users.isEmpty(), "startBackup", "users must not be empty");
    Q_ASSERT_X(!destinationPath.isEmpty(), "startBackup", "destinationPath must not be empty");
    Q_ASSERT_X(options.compression_level >= 0 && options.compression_level <= 9,
               "startBackup",
               "compressionLevel must be 0-9");
    if (m_running) {
        Q_EMIT logMessage(tr("Backup already in progress"), true);
        return;
    }

    QMutexLocker locker(&m_mutex);

    m_manifest = manifest;
    m_users = users;
    m_destinationPath = destinationPath;
    m_smartFilter = smartFilter;
    m_permissionMode = options.permission_mode;
    m_compressionLevel = options.compression_level;
    m_encrypt = options.encrypt;
    m_password = options.password;

    m_cancelled = false;
    m_bytesCopied = 0;
    m_filesCopied = 0;
    m_filesSkipped = 0;
    m_filesErrored = 0;
    m_filesElevationSkipped = 0;

    // Apply filter settings
    m_fileFilter->setRules(smartFilter);

    start();
}

void UserProfileBackupWorker::cancel() {
    m_cancelled = true;
    Q_EMIT logMessage(tr("Canceling backup..."), false);
}

void UserProfileBackupWorker::run() {
    Q_ASSERT(!m_users.empty());
    Q_ASSERT(!m_users.isEmpty());
    m_running = true;

    Q_EMIT logMessage(tr("=== Backup Started ==="), false);
    Q_EMIT logMessage(tr("Destination: %1").arg(m_destinationPath), false);
    Q_EMIT logMessage(tr("Users to backup: %1").arg(m_users.size()), false);

    // Validate inputs
    if (!validateSourcePaths()) {
        Q_EMIT backupComplete(false, tr("Invalid source paths"), m_manifest);
        m_running = false;
        return;
    }

    // Check disk space
    if (!checkDiskSpace()) {
        Q_EMIT backupComplete(false, tr("Insufficient disk space"), m_manifest);
        m_running = false;
        return;
    }

    // Calculate total size for progress
    Q_EMIT logMessage(tr("Calculating total size..."), false);
    m_totalBytesToCopy = calculateTotalSize();
    Q_EMIT logMessage(
        tr("Total estimated size: %1 GB").arg(m_totalBytesToCopy / sak::kBytesPerGBf, 0, 'f', 2),
        false);

    // Create backup directory structure
    if (!createBackupStructure()) {
        Q_EMIT backupComplete(false, tr("Failed to create backup structure"), m_manifest);
        m_running = false;
        return;
    }

    // Backup each user
    backupAllUsers();

    // Save manifest and emit summary
    emitBackupSummary();

    m_running = false;
}

void UserProfileBackupWorker::backupAllUsers() {
    Q_ASSERT(!m_users.empty());
    Q_ASSERT(!m_users.isEmpty());
    int userIndex = 0;
    for (const auto& user : m_users) {
        if (m_cancelled) {
            Q_EMIT logMessage(tr("Backup cancelled by user"), true);
            Q_EMIT backupComplete(false, tr("Backup cancelled"), m_manifest);
            m_running = false;
            return;
        }

        if (!user.is_selected) {
            continue;
        }

        Q_EMIT statusUpdate(user.username, tr("Starting backup..."));
        Q_EMIT logMessage(tr("=== Backing up user: %1 ===").arg(user.username), false);

        QString userBackupPath = m_destinationPath + "/" + user.username;
        if (!backupUser(user, userBackupPath)) {
            Q_EMIT logMessage(tr("Failed to backup user: %1").arg(user.username), true);
            // Continue with other users
        }

        userIndex++;
        Q_EMIT overallProgress(userIndex, m_users.size(), m_bytesCopied, m_totalBytesToCopy);
    }
}

void UserProfileBackupWorker::emitBackupSummary() {
    // Save manifest
    Q_EMIT logMessage(tr("Saving backup manifest..."), false);
    if (!saveManifest()) {
        Q_EMIT logMessage(tr("Warning: Failed to save manifest"), true);
    }

    // Complete
    QString summary = tr("Backup complete!\nFiles copied: %1\nFiles skipped: %2\n"
                         "Skipped (elevation required): %3\nErrors: %4\nTotal "
                         "size: %5 MB")
                          .arg(m_filesCopied)
                          .arg(m_filesSkipped)
                          .arg(m_filesElevationSkipped)
                          .arg(m_filesErrored)
                          .arg(m_bytesCopied / sak::kBytesPerMBf, 0, 'f', 1);

    Q_EMIT logMessage(tr("=== Backup Complete ==="), false);
    Q_EMIT logMessage(summary, false);
    Q_EMIT backupComplete(true, summary, m_manifest);
}

bool UserProfileBackupWorker::backupUser(const UserProfile& user, const QString& userBackupPath) {
    Q_ASSERT(!userBackupPath.isEmpty());
    // Create user backup directory
    QDir dir;
    if (!dir.mkpath(userBackupPath)) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(userBackupPath), true);
        return false;
    }

    // Backup each selected folder
    for (const auto& folder : user.folder_selections) {
        if (m_cancelled) {
            return false;
        }

        if (!folder.selected) {
            continue;
        }

        Q_EMIT statusUpdate(user.username, tr("Backing up: %1").arg(folder.display_name));

        QString sourcePath = user.profile_path + "/" + folder.relative_path;
        QString destPath = userBackupPath + "/" + folder.relative_path;

        if (!backupFolder(folder, sourcePath, destPath)) {
            Q_EMIT logMessage(tr("Warning: Failed to backup folder: %1").arg(folder.display_name),
                              true);
            // Continue with other folders
        }
    }

    return true;
}

bool UserProfileBackupWorker::backupFolder(const FolderSelection& folder,
                                           const QString& sourcePath,
                                           const QString& destPath) {
    Q_ASSERT_X(!sourcePath.isEmpty(), "backupFolder", "sourcePath must not be empty");
    Q_ASSERT_X(!destPath.isEmpty(), "backupFolder", "destPath must not be empty");
    QFileInfo sourceInfo(sourcePath);

    if (!sourceInfo.exists()) {
        Q_EMIT logMessage(tr("Source does not exist: %1").arg(sourcePath), true);
        return false;
    }

    if (sourceInfo.isDir()) {
        return copyDirectory(sourcePath, destPath, folder);
    }
    if (sourceInfo.isFile()) {
        return copyFileWithFiltering(sourcePath, destPath, sourceInfo.size());
    }

    return false;
}

bool UserProfileBackupWorker::copyDirectory(const QString& sourceDir,
                                            const QString& destDir,
                                            const FolderSelection& folderConfig) {
    Q_ASSERT_X(!sourceDir.isEmpty(), "copyDirectory", "sourceDir must not be empty");
    Q_ASSERT_X(!destDir.isEmpty(), "copyDirectory", "destDir must not be empty");
    // Check if folder should be excluded
    QFileInfo sourceDirInfo(sourceDir);
    QString currentUserProfile = m_users.isEmpty() ? QString() : m_users[0].profile_path;

    if (m_fileFilter->shouldExcludeFolder(sourceDirInfo, currentUserProfile)) {
        QString reason = m_fileFilter->getExclusionReason(sourceDirInfo);
        Q_EMIT logMessage(tr("Skipping folder: %1 (%2)").arg(sourceDir, reason), false);
        m_filesSkipped++;
        return true;  // Not an error, just skipped
    }

    // Check if directory is accessible (cross-user folders may need elevation)
    if (!canReadPath(sourceDir)) {
        Q_EMIT logMessage(tr("Skipping folder (requires elevation): %1").arg(sourceDir), true);
        Q_EMIT elevationSkipped(sourceDir, QString());
        m_filesElevationSkipped++;
        return true;
    }

    // Create destination directory
    if (!createDirectory(destDir)) {
        return false;
    }

    // Iterate through directory contents
    QDirIterator it(sourceDir,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    while (it.hasNext()) {
        if (m_cancelled) {
            return false;
        }

        QString sourceItem = it.next();
        QFileInfo itemInfo(sourceItem);
        QString destItem = destDir + "/" + itemInfo.fileName();

        if (itemInfo.isDir() && !copyDirectory(sourceItem, destItem, folderConfig)) {
            Q_EMIT logMessage(tr("Warning: Failed to copy directory: %1").arg(sourceItem), true);
        }

        if (itemInfo.isFile()) {
            copyFileWithFiltering(sourceItem, destItem, itemInfo.size());
        }
    }

    return true;
}

bool UserProfileBackupWorker::copyFileWithFiltering(const QString& sourcePath,
                                                    const QString& destPath,
                                                    qint64 fileSize) {
    Q_ASSERT_X(!sourcePath.isEmpty(), "copyFileWithFiltering", "sourcePath must not be empty");
    Q_ASSERT_X(!destPath.isEmpty(), "copyFileWithFiltering", "destPath must not be empty");
    Q_ASSERT_X(fileSize >= 0, "copyFileWithFiltering", "fileSize must be non-negative");
    QFileInfo sourceInfo(sourcePath);
    QString currentUserProfile = m_users.isEmpty() ? QString() : m_users[0].profile_path;

    // Apply smart filtering
    if (m_fileFilter->shouldExcludeFile(sourceInfo, currentUserProfile)) {
        QString reason = m_fileFilter->getExclusionReason(sourceInfo);
        Q_EMIT logMessage(tr("Skipping file: %1 (%2)").arg(sourceInfo.fileName(), reason), false);
        m_filesSkipped++;
        return true;  // Not an error
    }

    // Check size limit
    if (m_fileFilter->exceedsSizeLimit(fileSize)) {
        Q_EMIT logMessage(tr("Skipping large file: %1 (%2 MB)")
                              .arg(sourceInfo.fileName())
                              .arg(fileSize / sak::kBytesPerMBf, 0, 'f', 1),
                          true);
        m_filesSkipped++;
        return true;
    }

    // Check if file is accessible (cross-user files may require elevation)
    if (!canReadPath(sourcePath)) {
        QString owner;
        if (!ElevationManager::isElevated()) {
            owner = m_permissionManager->getOwner(sourcePath);
        }
        Q_EMIT logMessage(tr("Skipping file (requires elevation): %1").arg(sourceInfo.fileName()),
                          true);
        Q_EMIT elevationSkipped(sourcePath, owner);
        m_filesElevationSkipped++;
        return true;
    }

    // Ensure destination directory exists
    QFileInfo destInfo(destPath);
    if (!createDirectory(destInfo.absolutePath())) {
        return false;
    }

    // Copy the file
    if (!QFile::copy(sourcePath, destPath)) {
        Q_EMIT logMessage(tr("Error copying file: %1").arg(sourcePath), true);
        m_filesErrored++;
        return false;
    }

    // Apply permission strategy
    if (!applyPermissions(destPath)) {
        Q_EMIT logMessage(tr("Warning: Failed to adjust permissions: %1").arg(destPath), true);
        // Continue anyway
    }

    // Update progress
    m_filesCopied++;
    updateProgress(fileSize);

    return true;
}

bool UserProfileBackupWorker::applyPermissions(const QString& filePath) {
    Q_ASSERT(m_permissionManager);
    Q_ASSERT(!filePath.isEmpty());
    switch (m_permissionMode) {
    case PermissionMode::StripAll:
        return m_permissionManager->stripPermissions(filePath);

    case PermissionMode::PreserveOriginal:
        // Do nothing, keep original permissions
        return true;

    case PermissionMode::AssignToDestination:
        // This would be done during restore, not backup
        return true;

    case PermissionMode::Hybrid:
        // Try to preserve, fall back to strip on error
        if (!m_permissionManager->stripPermissions(filePath)) {
            return m_permissionManager->stripPermissions(filePath);
        }
        return true;
    }

    return true;
}

bool UserProfileBackupWorker::createBackupStructure() {
    QDir dir;

    // Create main backup directory
    if (!dir.mkpath(m_destinationPath)) {
        Q_EMIT logMessage(tr("Failed to create backup directory: %1").arg(m_destinationPath), true);
        return false;
    }

    Q_EMIT logMessage(tr("Created backup directory: %1").arg(m_destinationPath), false);
    return true;
}

bool UserProfileBackupWorker::saveManifest() {
    // Update manifest with actual results
    m_manifest.total_backup_size_bytes = m_bytesCopied;
    m_manifest.created = QDateTime::currentDateTime();

    // Add user data to manifest
    m_manifest.users.clear();
    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }

        BackupUserData userData;
        userData.username = user.username;
        userData.sid = user.sid;
        userData.profile_path = user.profile_path;
        userData.backed_up_folders = user.folder_selections;
        userData.permissions_mode = m_permissionMode;

        m_manifest.users.append(userData);
    }

    // Save to JSON file
    QString manifestPath = m_destinationPath + "/manifest.json";
    return m_manifest.saveToFile(manifestPath);
}

qint64 UserProfileBackupWorker::accumulateUserFolderSizes(const UserProfile& user) {
    qint64 size = 0;
    for (const auto& folder : user.folder_selections) {
        if (!folder.selected) {
            continue;
        }
        size += folder.size_bytes;
        m_totalFilesToCopy += folder.file_count;
    }
    return size;
}

qint64 UserProfileBackupWorker::calculateTotalSize() {
    qint64 totalSize = 0;
    m_totalFilesToCopy = 0;

    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }
        totalSize += accumulateUserFolderSizes(user);
    }

    return totalSize;
}

void UserProfileBackupWorker::countSelectedUsers(int& currentUser, int& totalUsers) const {
    totalUsers = 0;
    currentUser = 0;
    for (int i = 0; i < m_users.size(); ++i) {
        if (!m_users[i].is_selected) {
            continue;
        }
        totalUsers++;
        if (i < m_users.size() - 1) {
            currentUser++;
        }
    }
}

void UserProfileBackupWorker::updateProgress(qint64 bytesAdded) {
    Q_ASSERT(bytesAdded >= 0);
    m_bytesCopied += bytesAdded;

    // Emit progress every 100 files or 100MB
    static int lastFileCount = 0;
    static qint64 lastByteCount = 0;

    if (m_filesCopied - lastFileCount >= 100 ||
        m_bytesCopied - lastByteCount >= 100 * sak::kBytesPerMB) {
        int currentUser = 0, totalUsers = 0;
        countSelectedUsers(currentUser, totalUsers);

        Q_EMIT overallProgress(currentUser, totalUsers, m_bytesCopied, m_totalBytesToCopy);
        Q_EMIT fileProgress(m_filesCopied, m_totalFilesToCopy);

        lastFileCount = m_filesCopied;
        lastByteCount = m_bytesCopied;
    }
}

bool UserProfileBackupWorker::createDirectory(const QString& path) {
    QDir dir;
    if (!dir.mkpath(path)) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(path), true);
        return false;
    }
    return true;
}

bool UserProfileBackupWorker::canReadPath(const QString& path) {
#ifdef Q_OS_WIN
    DWORD attrs = GetFileAttributesW(path.toStdWString().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        return err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION;
    }
    // Try opening for read access to confirm
    HANDLE handle = CreateFileW(path.toStdWString().c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                (attrs & FILE_ATTRIBUTE_DIRECTORY) ? FILE_FLAG_BACKUP_SEMANTICS : 0,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() != ERROR_ACCESS_DENIED;
    }
    CloseHandle(handle);
    return true;
#else
    return QFileInfo(path).isReadable();
#endif
}

bool UserProfileBackupWorker::validateSourcePaths() {
    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }

        QDir profileDir(user.profile_path);
        if (!profileDir.exists()) {
            Q_EMIT logMessage(tr("User profile does not exist: %1").arg(user.profile_path), true);
            return false;
        }
    }

    return true;
}

bool UserProfileBackupWorker::checkDiskSpace() {
    QStorageInfo storage(m_destinationPath);

    if (!storage.isValid() || !storage.isReady()) {
        Q_EMIT logMessage(tr("Invalid or not ready destination: %1").arg(m_destinationPath), true);
        return false;
    }

    qint64 availableBytes = storage.bytesAvailable();
    qint64 requiredBytes = m_totalBytesToCopy;

    // Add 10% buffer for overhead
    requiredBytes = requiredBytes * 1.1;

    if (availableBytes < requiredBytes) {
        double availableGB = availableBytes / sak::kBytesPerGBf;
        double requiredGB = requiredBytes / sak::kBytesPerGBf;

        Q_EMIT logMessage(tr("Insufficient disk space. Available: %1 GB, Required: %2 GB")
                              .arg(availableGB, 0, 'f', 1)
                              .arg(requiredGB, 0, 'f', 1),
                          true);
        return false;
    }

    Q_EMIT logMessage(tr("Disk space check passed"), false);
    return true;
}

}  // namespace sak
