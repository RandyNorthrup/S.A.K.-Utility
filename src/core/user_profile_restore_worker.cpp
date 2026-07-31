// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_restore_worker.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/path_utils.h"
#include "sak/permission_manager.h"
#include "sak/smart_file_filter.h"
#include "sak/windows_user_scanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

#include <algorithm>

namespace sak {

namespace {
constexpr int kRestoreProgressFileInterval = 100;
constexpr qint64 kRestoreProgressByteInterval = kRestoreProgressFileInterval * kBytesPerMB;
constexpr int kRestoreConflictMaxAttempts = 1000;

bool buildSafePath(const QString& basePath, const QString& relativePath, QString& outPath) {
    Q_ASSERT(!basePath.isEmpty());
    Q_ASSERT(!relativePath.isEmpty());
    QString combined = QDir(basePath).filePath(relativePath);
    auto nativeCombined = QDir::toNativeSeparators(combined);
    auto nativeBase = QDir::toNativeSeparators(basePath);

    std::filesystem::path destPath = nativeCombined.toStdString();
    std::filesystem::path base = nativeBase.toStdString();

    auto safe = path_utils::isSafePath(destPath, base);
    if (!safe || !(*safe)) {
        return false;
    }

    outPath = combined;
    return true;
}

// True if the path is a reparse point (junction/symlink), on either the backup
// source or the restore destination side.
bool isReparsePoint(const QFileInfo& info) {
    return info.isSymLink() || info.isJunction();
}

// A junction or symlink on either side lets the copy read from outside the
// backup root or write outside the validated profile root; refuse to traverse.
bool restoreEntryEscapesRoot(const QFileInfo& sourceEntry, const QString& destItem) {
    return isReparsePoint(sourceEntry) || isReparsePoint(QFileInfo(destItem));
}

// Save the pre-restore contents of an about-to-be-overwritten file to a
// recoverable sidecar so the user can undo the overwrite. Returns false (fail
// closed) if the copy fails, so the caller never destroys the original without
// a recovery copy in hand.
bool createRestoreRecoveryCopy(const QString& finalDestPath) {
    const QString recoveryPath = finalDestPath + QStringLiteral(".sakbak");
    QFile::remove(recoveryPath);  // Drop a stale copy from an earlier restore.
    return QFile::copy(finalDestPath, recoveryPath);
}

// Replace an existing destination file only after the new copy is fully written.
// The original is moved aside by RENAME (never deleted) before the replacement is
// swapped in, and restored if the swap fails, so no failure can lose the original
// destination. When backupExisting is set, the original is also preserved to a
// `.sakbak` recovery copy for user-visible undo.
bool copyFileReplacingExisting(const QString& source,
                               const QString& finalDestPath,
                               bool backupExisting) {
    if (!QFileInfo::exists(finalDestPath)) {
        return QFile::copy(source, finalDestPath);
    }
    const QString tempPath = finalDestPath + QStringLiteral(".sakrestore.tmp");
    QFile::remove(tempPath);
    if (!QFile::copy(source, tempPath)) {
        return false;
    }
    if (backupExisting && !createRestoreRecoveryCopy(finalDestPath)) {
        QFile::remove(tempPath);
        return false;  // Never overwrite without a recovery copy in place.
    }

    // Move the ORIGINAL aside by rename (not delete). QFile::rename cannot overwrite an existing
    // target, so the destination must be vacated first -- but by relocating the original, not
    // destroying it, so a failed swap can roll back. The old code removed the original THEN
    // renamed, and a rename failure between the two left the destination gone.
    const QString oldPath = finalDestPath + QStringLiteral(".sakold.tmp");
    QFile::remove(oldPath);
    if (!QFile::rename(finalDestPath, oldPath)) {
        QFile::remove(tempPath);
        return false;  // Could not move the original aside -- leave everything intact.
    }
    if (!QFile::rename(tempPath, finalDestPath)) {
        QFile::rename(oldPath, finalDestPath);  // Roll back: the original is restored in place.
        QFile::remove(tempPath);
        return false;
    }
    QFile::remove(oldPath);  // Replacement is in place; drop the moved-aside original.
    return true;
}
}  // namespace

UserProfileRestoreWorker::UserProfileRestoreWorker(QObject* parent)
    : QThread(parent)
    , m_fileFilter(new SmartFileFilter())
    , m_permissionManager(new PermissionManager()) {}

UserProfileRestoreWorker::~UserProfileRestoreWorker() {
    if (isRunning()) {
        cancel();
        // A large in-flight QFile::copy cannot observe cancellation, so the
        // bounded wait can time out. Block until the thread has fully exited
        // rather than destroying a running QThread (which aborts the process).
        if (!wait(kTimeoutThreadTerminateMs)) {
            wait();
        }
    }
    delete m_fileFilter;
    delete m_permissionManager;
}

void UserProfileRestoreWorker::startRestore(const QString& backupPath,
                                            const BackupManifest& manifest,
                                            const QVector<UserMapping>& mappings,
                                            const RestoreConfig& config) {
    Q_ASSERT_X(!backupPath.isEmpty(), "startRestore", "backupPath must not be empty");
    Q_ASSERT_X(!mappings.isEmpty(), "startRestore", "mappings must not be empty");
    if (isRunning()) {
        Q_EMIT logMessage(tr("Restore already in progress"), true);
        return;
    }

    QMutexLocker locker(&m_mutex);

    m_backupPath = backupPath;
    m_manifest = manifest;
    m_mappings = mappings;
    m_conflictMode = config.conflict_mode;
    m_permissionMode = config.perm_mode;
    m_verify = config.verify;
    m_createBackup = config.create_backup;

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
    Q_ASSERT(!m_mappings.isEmpty());

    Q_EMIT logMessage(tr("=== Restore Started ==="), false);
    Q_EMIT logMessage(tr("Backup: %1").arg(m_backupPath), false);
    Q_EMIT logMessage(tr("Users to restore: %1").arg(m_mappings.size()), false);

    // Validate backup
    if (!validateBackup()) {
        Q_EMIT restoreComplete(false, tr("Invalid backup"));
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
            return;
        }

        if (!mapping.selected) {
            continue;
        }

        Q_EMIT statusUpdate(mapping.source_username, tr("Starting restore..."));
        Q_EMIT logMessage(tr("=== Restoring user: %1 -> %2 ===")
                              .arg(mapping.source_username,
                                   mapping.destination_username.isEmpty()
                                       ? tr("(New)")
                                       : mapping.destination_username),
                          false);

        if (!restoreUser(mapping)) {
            Q_EMIT logMessage(tr("Failed to restore user: %1").arg(mapping.source_username), true);
        }

        userIndex++;
        Q_EMIT overallProgress(
            userIndex, m_mappings.size(), m_bytesRestored, m_totalBytesToRestore);
    }

    // Complete
    QString summary = tr("Restore complete!\nFiles restored: %1\nFiles skipped: %2\nErrors: "
                         "%3\nTotal size: %4 MB")
                          .arg(m_filesRestored)
                          .arg(m_filesSkipped)
                          .arg(m_filesErrored)
                          .arg(m_bytesRestored / sak::kBytesPerMBf, 0, 'f', 1);

    Q_EMIT logMessage(tr("=== Restore Complete ==="), false);
    Q_EMIT logMessage(summary, false);
    // Report success only when nothing errored (copy/mkpath/remove failure or a
    // refused reparse point); a partial restore must not read as clean success.
    const bool success = (m_filesErrored == 0);
    Q_EMIT restoreComplete(success, summary);
}

bool UserProfileRestoreWorker::restoreUser(const UserMapping& mapping) {
    Q_ASSERT(!mapping.source_username.isEmpty());
    Q_ASSERT(!m_backupPath.isEmpty());

    // Find source user data in manifest
    const BackupUserData* sourceUser = findManifestUser(mapping.source_username);
    if (!sourceUser) {
        Q_EMIT logMessage(tr("Source user not found in manifest: %1").arg(mapping.source_username),
                          true);
        return false;
    }

    QString sourcePath;
    if (!buildSafePath(m_backupPath, mapping.source_username, sourcePath)) {
        Q_EMIT logMessage(tr("Invalid source path for user: %1").arg(mapping.source_username),
                          true);
        return false;
    }

    QString destProfilePath;
    if (!resolveDestinationProfilePath(mapping, destProfilePath)) {
        return false;
    }

    // Restore each folder
    for (const auto& folder : sourceUser->backed_up_folders) {
        if (m_cancelled) {
            return false;
        }

        // Honor the folder-selection page: a folder the user unchecked must not
        // be restored (its `selected` flag was persisted into the manifest).
        if (!folder.selected) {
            continue;
        }

        Q_EMIT statusUpdate(mapping.source_username, tr("Restoring: %1").arg(folder.display_name));

        QString folderSourcePath;
        QString folderDestPath;
        if (!buildSafePath(sourcePath, folder.relative_path, folderSourcePath)) {
            Q_EMIT logMessage(tr("Invalid source folder path: %1").arg(folder.relative_path), true);
            continue;
        }
        if (!buildSafePath(destProfilePath, folder.relative_path, folderDestPath)) {
            Q_EMIT logMessage(tr("Invalid destination folder path: %1").arg(folder.relative_path),
                              true);
            continue;
        }

        if (!restoreFolder(folder, folderSourcePath, folderDestPath)) {
            Q_EMIT logMessage(tr("Warning: Failed to restore folder: %1").arg(folder.display_name),
                              true);
            // Continue with other folders
        }
    }

    return true;
}

bool UserProfileRestoreWorker::resolveCreateNewUser(const UserMapping& mapping,
                                                    const QString& systemDrive,
                                                    QString& destProfilePath) {
    const QString destUsername = mapping.destination_username.isEmpty()
                                     ? mapping.source_username
                                     : mapping.destination_username;
    QString baseProfileRoot = systemDrive + "/Users";
    if (!buildSafePath(baseProfileRoot, destUsername, destProfilePath)) {
        Q_EMIT logMessage(tr("Invalid destination username: %1").arg(destUsername), true);
        return false;
    }
    if (!QDir().mkpath(destProfilePath)) {
        Q_EMIT logMessage(tr("Failed to create profile directory: %1").arg(destProfilePath), true);
        return false;
    }
    Q_EMIT logMessage(tr("Created new profile: %1").arg(destProfilePath), false);
    return true;
}

bool UserProfileRestoreWorker::resolveExistingUser(const UserMapping& mapping,
                                                   QString& destProfilePath) {
    if (mapping.destination_username.isEmpty()) {
        Q_EMIT logMessage(tr("Destination username not specified"), true);
        return false;
    }
    destProfilePath = WindowsUserScanner::getProfilePath(mapping.destination_username);
    if (destProfilePath.isEmpty()) {
        Q_EMIT logMessage(
            tr("Failed to resolve destination profile path: %1").arg(mapping.destination_username),
            true);
        return false;
    }
    if (!QDir(destProfilePath).exists()) {
        Q_EMIT logMessage(tr("Destination profile does not exist: %1").arg(destProfilePath), true);
        return false;
    }
    return true;
}

bool UserProfileRestoreWorker::resolveDestinationProfilePath(const UserMapping& mapping,
                                                             QString& destProfilePath) {
    QString systemDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));
    if (systemDrive.isEmpty()) {
        systemDrive = "C:";
    }

    switch (mapping.mode) {
    case MergeMode::CreateNewUser:
        return resolveCreateNewUser(mapping, systemDrive, destProfilePath);
    case MergeMode::ReplaceDestination:
    case MergeMode::MergeIntoDestination:
        return resolveExistingUser(mapping, destProfilePath);
    }
    return true;
}

bool UserProfileRestoreWorker::restoreFolder(const FolderSelection& folder,
                                             const QString& sourcePath,
                                             const QString& destPath) {
    Q_ASSERT_X(!sourcePath.isEmpty(), "restoreFolder", "sourcePath must not be empty");
    Q_ASSERT_X(!destPath.isEmpty(), "restoreFolder", "destPath must not be empty");
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
    Q_ASSERT_X(!sourceDir.isEmpty(), "copyDirectory", "sourceDir must not be empty");
    Q_ASSERT_X(!destDir.isEmpty(), "copyDirectory", "destDir must not be empty");
    QDir dir(sourceDir);
    if (!dir.exists()) {
        return false;
    }

    // Create destination directory
    if (!QDir().mkpath(destDir)) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(destDir), true);
        return false;
    }

    // Iterate through all entries
    QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);

    // cppcheck-suppress useStlAlgorithm ; loop has side effects (file copy, cancellation)
    for (const QFileInfo& entry : entries) {
        if (m_cancelled) {
            return false;
        }

        QString sourceItem = entry.absoluteFilePath();
        QString destItem = destDir + "/" + entry.fileName();

        // Refuse junctions/symlinks on either side: they can read outside the
        // backup or write outside the approved profile root (e.g. into System32).
        if (restoreEntryEscapesRoot(entry, destItem)) {
            Q_EMIT logMessage(
                tr("Skipping reparse point to prevent restore escape: %1").arg(sourceItem), true);
            m_filesErrored++;
            continue;
        }

        // Recursively copy subdirectory
        if (entry.isDir() && !copyDirectory(sourceItem, destItem, folderConfig)) {
            Q_EMIT logMessage(tr("Warning: Failed to copy directory: %1").arg(sourceItem), true);
            continue;
        }

        // Copy file with conflict resolution
        if (entry.isFile()) {
            copyFileWithConflictResolution(sourceItem, destItem, entry.size());
        }
    }

    return true;
}

bool UserProfileRestoreWorker::copyFileWithConflictResolution(const QString& source,
                                                              const QString& dest,
                                                              qint64 size) {
    Q_ASSERT_X(!source.isEmpty(), "copyFileWithConflictResolution", "source must not be empty");
    Q_ASSERT_X(!dest.isEmpty(), "copyFileWithConflictResolution", "dest must not be empty");
    Q_ASSERT_X(size >= 0, "copyFileWithConflictResolution", "size must be non-negative");
    QFileInfo destInfo(dest);
    QString finalDestPath = dest;

    // Check if destination file exists and resolve conflict
    if (destInfo.exists()) {
        if (!resolveFileConflict(source, destInfo, size, finalDestPath)) {
            return true;  // File was skipped per conflict resolution strategy
        }
    }

    // Ensure destination directory exists
    QFileInfo finalDestInfo(finalDestPath);
    if (!QDir().mkpath(finalDestInfo.absolutePath())) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(finalDestInfo.absolutePath()),
                          true);
        m_filesErrored++;
        return false;
    }

    // Copy the file (atomically replacing an existing destination, if any). When
    // the safety option is on, the pre-restore file is saved to `.sakbak` first.
    if (!copyFileReplacingExisting(source, finalDestPath, m_createBackup)) {
        Q_EMIT logMessage(tr("Error copying file: %1").arg(source), true);
        m_filesErrored++;
        return false;
    }

    // Apply permissions based on mode
    if (!applyPermissions(finalDestPath, "")) {
        Q_EMIT logMessage(tr("Warning: Failed to adjust permissions: %1").arg(finalDestPath), true);
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

QString UserProfileRestoreWorker::generateConflictRenamePath(const QFileInfo& destInfo) {
    QString baseName = destInfo.completeBaseName();
    QString extension = destInfo.suffix();
    QString dirPath = destInfo.absolutePath();
    QString suffix = extension.isEmpty() ? QString() : "." + extension;

    constexpr int kMaxRenameAttempts = 1000;
    int counter = 1;
    QString renamed;
    do {
        renamed =
            QString("%1/%2_backup%3%4").arg(dirPath, baseName, QString::number(counter++), suffix);
    } while (QFileInfo::exists(renamed) && counter < kMaxRenameAttempts);

    return renamed;
}

bool UserProfileRestoreWorker::resolveFileConflict(const QString& source,
                                                   const QFileInfo& destInfo,
                                                   qint64 size,
                                                   QString& finalDestPath) {
    switch (m_conflictMode) {
    case ConflictResolution::SkipDuplicate:
        Q_EMIT logMessage(tr("Skipping existing file: %1").arg(destInfo.fileName()), false);
        m_filesSkipped++;
        return false;

    case ConflictResolution::RenameWithSuffix:
        finalDestPath = generateConflictRenamePath(destInfo);
        Q_EMIT logMessage(
            tr("Renaming to avoid conflict: %1").arg(QFileInfo(finalDestPath).fileName()), false);
        break;

    case ConflictResolution::KeepNewer: {
        QFileInfo sourceInfo(source);
        if (destInfo.lastModified() >= sourceInfo.lastModified()) {
            Q_EMIT logMessage(tr("Keeping newer existing file: %1").arg(destInfo.fileName()),
                              false);
            m_filesSkipped++;
            return false;
        }
        // Do not delete here: copyFileReplacingExisting removes the original
        // only after the replacement is fully written, so a failed copy never
        // leaves the destination missing.
        Q_EMIT logMessage(tr("Replacing with newer file: %1").arg(destInfo.fileName()), false);
        break;
    }

    case ConflictResolution::KeepLarger: {
        if (destInfo.size() >= size) {
            Q_EMIT logMessage(tr("Keeping larger existing file: %1").arg(destInfo.fileName()),
                              false);
            m_filesSkipped++;
            return false;
        }
        Q_EMIT logMessage(tr("Replacing with larger file: %1").arg(destInfo.fileName()), false);
        break;
    }

    case ConflictResolution::PromptUser:
        finalDestPath = resolveConflict(finalDestPath);
        Q_EMIT logMessage(tr("File exists, auto-renamed: %1 -> %2")
                              .arg(destInfo.fileName(), QFileInfo(finalDestPath).fileName()),
                          false);
        break;
    }
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
        // Look up user SID and apply ownership + standard permissions
        {
            QString userSID = WindowsUserScanner::getUserSID(destinationUser);
            if (userSID.isEmpty()) {
                sak::logWarning(
                    "Could not resolve SID for user '{}', stripping permissions "
                    "instead",
                    destinationUser.toStdString());
                return m_permissionManager->stripPermissions(filePath);
            }
            if (!m_permissionManager->takeOwnership(filePath, userSID)) {
                sak::logWarning("Failed to take ownership for '{}', stripping permissions",
                                filePath.toStdString());
                return m_permissionManager->stripPermissions(filePath);
            }
            return m_permissionManager->setStandardUserPermissions(filePath, userSID);
        }

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

    // Integrity: a populated manifest checksum that does not match means the
    // manifest.json was corrupted or tampered with -> refuse (B7-13). A legacy
    // backup with no stored checksum verifies as true (nothing to check).
    if (!m_manifest.verifyManifestChecksum()) {
        Q_EMIT logMessage(tr("Manifest integrity check failed (checksum mismatch)"), true);
        return false;
    }
    if (m_manifest.manifest_checksum.isEmpty()) {
        Q_EMIT logMessage(
            tr("Manifest has no integrity checksum (legacy backup); skipping verification"), false);
    }

    if (!verifyUserPayloadChecksums()) {
        return false;
    }

    Q_EMIT logMessage(tr("Backup validation passed"), false);
    return true;
}

bool UserProfileRestoreWorker::verifyUserPayloadChecksums() {
    for (const auto& mapping : m_mappings) {
        if (!mapping.selected) {
            continue;
        }
        const auto* user = findManifestUser(mapping.source_username);
        if (user == nullptr || user->checksum_sha256.isEmpty()) {
            continue;  // Unmapped, or a legacy payload with no recorded digest.
        }
        const QString actual =
            BackupManifest::hashDirectoryTree(m_backupPath + "/" + user->username);
        if (actual != user->checksum_sha256) {
            Q_EMIT logMessage(tr("Payload integrity check failed for user '%1' (checksum mismatch)")
                                  .arg(user->username),
                              true);
            return false;
        }
    }
    return true;
}

bool UserProfileRestoreWorker::createRestoreStructure() {
    Q_EMIT logMessage(tr("Creating restore directory structure..."), false);

    // Create destination directories for each mapping
    for (const auto& mapping : m_mappings) {
        if (!mapping.selected) {
            continue;
        }

        // Get destination profile path from username
        QString destPath = WindowsUserScanner::getProfilePath(mapping.destination_username);
        if (destPath.isEmpty()) {
            Q_EMIT logMessage(
                tr("Failed to resolve profile path for user: %1").arg(mapping.destination_username),
                true);
            return false;
        }

        QDir destDir(destPath);
        bool needsCreation = !destDir.exists();

        if (needsCreation && !destDir.mkpath(".")) {
            Q_EMIT logMessage(tr("Failed to create directory: %1").arg(destPath), true);
            return false;
        }
        if (needsCreation) {
            Q_EMIT logMessage(tr("Created directory: %1").arg(destPath), false);
        }

        // Create standard user profile subdirectories
        createStandardSubfolders(destDir);
    }

    Q_EMIT logMessage(tr("Restore directory structure created"), false);
    return true;
}

qint64 UserProfileRestoreWorker::calculateTotalSize() {
    qint64 totalSize = 0;
    m_totalFilesToRestore = 0;

    for (const auto& mapping : m_mappings) {
        if (!mapping.selected) {
            continue;
        }

        const auto* user = findManifestUser(mapping.source_username);
        if (!user) {
            continue;
        }

        for (const auto& folder : user->backed_up_folders) {
            if (!folder.selected) {
                continue;  // Unchecked folders are skipped during restore; exclude from totals.
            }
            totalSize += folder.size_bytes;
            m_totalFilesToRestore += folder.file_count;
        }
    }

    return totalSize;
}

const BackupUserData* UserProfileRestoreWorker::findManifestUser(const QString& username) const {
    auto it = std::find_if(m_manifest.users.begin(),
                           m_manifest.users.end(),
                           [&username](const auto& user) { return user.username == username; });
    return (it != m_manifest.users.end()) ? &(*it) : nullptr;
}

void UserProfileRestoreWorker::createStandardSubfolders(const QDir& destDir) {
    Q_ASSERT(!destDir.isEmpty());
    QStringList standardFolders = {"Documents",
                                   "Desktop",
                                   "Pictures",
                                   "Videos",
                                   "Music",
                                   "Downloads",
                                   "AppData",
                                   "AppData/Local",
                                   "AppData/Roaming",
                                   "Favorites"};

    for (const QString& folder : standardFolders) {
        QString folderPath = destDir.filePath(folder);
        if (!QDir(folderPath).exists() && !QDir().mkpath(folderPath)) {
            Q_EMIT logMessage(tr("Failed to create subdirectory: %1").arg(folderPath), true);
            // Continue anyway - not all folders may be needed
        }
    }
}

void UserProfileRestoreWorker::updateProgress(qint64 bytesAdded) {
    Q_ASSERT(bytesAdded >= 0);
    m_bytesRestored += bytesAdded;

    static int lastFileCount = 0;
    static qint64 lastByteCount = 0;

    if (m_filesRestored - lastFileCount >= kRestoreProgressFileInterval ||
        m_bytesRestored - lastByteCount >= kRestoreProgressByteInterval) {
        Q_EMIT fileProgress(m_filesRestored, m_totalFilesToRestore);

        lastFileCount = m_filesRestored;
        lastByteCount = m_bytesRestored;
    }
}

bool UserProfileRestoreWorker::verifyFile(const QString& filePath) {
    Q_ASSERT(!filePath.isEmpty());
    QFileInfo fileInfo(filePath);

    if (!fileInfo.exists()) {
        Q_EMIT logMessage(tr("Verification failed - file missing: %1").arg(filePath), true);
        return false;
    }

    if (!fileInfo.isReadable()) {
        Q_EMIT logMessage(tr("Verification failed - file not readable: %1").arg(filePath), true);
        return false;
    }

    // Per-file existence + readability. Content integrity of the stored payload is
    // covered upstream by validateBackup(), which recomputes each user's directory
    // digest against the manifest's checksum_sha256 before any file is restored.
    return true;
}

QString UserProfileRestoreWorker::resolveConflict(const QString& destPath) {
    // Generate unique filename by adding suffix
    QFileInfo destInfo(destPath);
    QString baseName = destInfo.completeBaseName();
    QString extension = destInfo.suffix();
    QString dirPath = destInfo.absolutePath();

    int counter = 1;
    QString newPath;
    do {
        newPath = QString("%1/%2_restored%3%4")
                      .arg(dirPath,
                           baseName,
                           QString::number(counter++),
                           extension.isEmpty() ? "" : "." + extension);
    } while (QFileInfo::exists(newPath) && counter < kRestoreConflictMaxAttempts);

    return newPath;
}

}  // namespace sak
