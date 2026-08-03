// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_restore_worker.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/path_utils.h"
#include "sak/permission_manager.h"
#include "sak/smart_file_filter.h"
#include "sak/windows_user_scanner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

#include <algorithm>
#include <limits>

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

    // Build the filesystem paths from UTF-16, not a lossy narrow (system-ANSI)
    // std::string: toStdString() would mangle any non-ASCII path component and
    // could make the containment check compare corrupted bytes.
    std::filesystem::path destPath(nativeCombined.toStdU16String());
    std::filesystem::path base(nativeBase.toStdU16String());

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
    m_lastProgressFileCount = 0;
    m_lastProgressByteCount = 0;

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
            // A whole-user setup failure (missing manifest entry, invalid/undefined
            // destination) must fail the overall result, not read as clean success.
            m_filesErrored++;
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

    // The effective destination user for permission assignment: an explicit
    // destination, else the source (a same-name restore). Consumed by
    // copyFileWithConflictResolution -> applyPermissions (B7-21).
    m_currentDestUser = effectiveDestUser(mapping);

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
            m_filesErrored++;  // A rejected folder must fail the overall result.
            continue;
        }
        if (!buildSafePath(destProfilePath, folder.relative_path, folderDestPath)) {
            Q_EMIT logMessage(tr("Invalid destination folder path: %1").arg(folder.relative_path),
                              true);
            m_filesErrored++;
            continue;
        }

        if (!restoreFolder(folder, folderSourcePath, folderDestPath)) {
            Q_EMIT logMessage(tr("Warning: Failed to restore folder: %1").arg(folder.display_name),
                              true);
            m_filesErrored++;  // Folder-level failure -> overall restore is not clean.
            // Continue with other folders
        }
    }

    return true;
}

bool UserProfileRestoreWorker::resolveCreateNewUser(const UserMapping& mapping,
                                                    const QString& systemDrive,
                                                    QString& destProfilePath) {
    if (systemDrive.isEmpty()) {
        Q_EMIT logMessage(tr("SystemDrive is not set; cannot resolve a new-user profile location"),
                          true);
        return false;
    }
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
    // No "C:" fallback: an unset SystemDrive is surfaced by resolveCreateNewUser
    // (the only branch that needs it), rather than silently guessing a drive.
    const QString systemDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));

    switch (mapping.mode) {
    case MergeMode::CreateNewUser:
        return resolveCreateNewUser(mapping, systemDrive, destProfilePath);
    case MergeMode::ReplaceDestination:
    case MergeMode::MergeIntoDestination:
        return resolveExistingUser(mapping, destProfilePath);
    }
    // Unknown/out-of-range merge mode: fail closed instead of returning success
    // with an unset destination path.
    Q_EMIT logMessage(tr("Unknown merge mode for user: %1").arg(mapping.source_username), true);
    return false;
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
            m_filesErrored++;  // A subtree that failed to copy must not read as success.
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

    // Apply permissions based on mode. Pass the current mapping's destination user
    // so AssignToDestination assigns ownership instead of always stripping (B7-21).
    // Fail CLOSED: a file whose ownership/ACL could not be set is NOT a clean restore.
    const bool permsOk = applyPermissions(finalDestPath, m_currentDestUser);
    if (!permsOk) {
        Q_EMIT logMessage(tr("Error: Failed to adjust permissions: %1").arg(finalDestPath), true);
    }

    // Verify file content against the source if requested. A verification failure
    // means the copied bytes differ or are unreadable -> count it as an error.
    const bool verifyOk = !m_verify || verifyFile(source, finalDestPath);
    if (!verifyOk) {
        Q_EMIT logMessage(tr("Error: File verification failed: %1").arg(finalDestPath), true);
    }

    if (!permsOk || !verifyOk) {
        m_filesErrored++;
        return false;
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

    // Exhausted every candidate and the last one still exists: return empty so the
    // caller fails closed instead of overwriting an existing unrelated file.
    if (QFileInfo::exists(renamed)) {
        return {};
    }
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
        return applyRenameTarget(generateConflictRenamePath(destInfo), destInfo, finalDestPath);

    case ConflictResolution::KeepNewer:
        // Do not delete here: copyFileReplacingExisting removes the original only
        // after the replacement is fully written, so a failed copy never leaves
        // the destination missing.
        if (destInfo.lastModified() >= QFileInfo(source).lastModified()) {
            Q_EMIT logMessage(tr("Keeping newer existing file: %1").arg(destInfo.fileName()),
                              false);
            m_filesSkipped++;
            return false;
        }
        Q_EMIT logMessage(tr("Replacing with newer file: %1").arg(destInfo.fileName()), false);
        return true;

    case ConflictResolution::KeepLarger:
        if (destInfo.size() >= size) {
            Q_EMIT logMessage(tr("Keeping larger existing file: %1").arg(destInfo.fileName()),
                              false);
            m_filesSkipped++;
            return false;
        }
        Q_EMIT logMessage(tr("Replacing with larger file: %1").arg(destInfo.fileName()), false);
        return true;

    case ConflictResolution::PromptUser:
        return applyRenameTarget(resolveConflict(finalDestPath), destInfo, finalDestPath);
    }
    // Unknown/out-of-range conflict mode: fail closed rather than overwrite the
    // original with finalDestPath left pointing at the existing destination.
    Q_EMIT logMessage(tr("Unknown conflict resolution mode; skipping: %1").arg(destInfo.fileName()),
                      true);
    m_filesErrored++;
    return false;
}

bool UserProfileRestoreWorker::applyRenameTarget(const QString& candidate,
                                                 const QFileInfo& destInfo,
                                                 QString& finalDestPath) {
    if (candidate.isEmpty()) {
        Q_EMIT logMessage(
            tr("Could not find a free rename target for: %1").arg(destInfo.fileName()), true);
        m_filesErrored++;
        return false;
    }
    finalDestPath = candidate;
    Q_EMIT logMessage(
        tr("Resolving conflict, restoring as: %1").arg(QFileInfo(candidate).fileName()), false);
    return true;
}

UserProfileRestoreWorker::PermissionAction UserProfileRestoreWorker::resolvePermissionAction(
    PermissionMode mode, const QString& destinationUser) {
    switch (mode) {
    case PermissionMode::PreserveOriginal:
        return PermissionAction::PreserveOriginal;
    case PermissionMode::AssignToDestination:
        // The whole point of this mode: assign ownership to the target user. Only
        // fall back to stripping when no user is known (B7-21).
        return destinationUser.isEmpty() ? PermissionAction::StripPermissions
                                         : PermissionAction::AssignOwnership;
    case PermissionMode::StripAll:
    case PermissionMode::Hybrid:
        return PermissionAction::StripPermissions;
    }
    return PermissionAction::StripPermissions;
}

QString UserProfileRestoreWorker::effectiveDestUser(const UserMapping& mapping) {
    return mapping.destination_username.isEmpty() ? mapping.source_username
                                                  : mapping.destination_username;
}

bool UserProfileRestoreWorker::assignOwnershipToUser(const QString& filePath,
                                                     const QString& destinationUser) {
    const QString userSID = WindowsUserScanner::getUserSID(destinationUser);
    if (userSID.isEmpty()) {
        sak::logWarning("Could not resolve SID for user '{}', stripping permissions instead",
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

bool UserProfileRestoreWorker::applyPermissions(const QString& filePath,
                                                const QString& destinationUser) {
    switch (resolvePermissionAction(m_permissionMode, destinationUser)) {
    case PermissionAction::PreserveOriginal:
        return true;  // Keep permissions already set by the copy.
    case PermissionAction::StripPermissions:
        return m_permissionManager->stripPermissions(filePath);
    case PermissionAction::AssignOwnership:
        return assignOwnershipToUser(filePath, destinationUser);
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
            accumulateFolderTotals(folder, totalSize);
        }
    }

    return totalSize;
}

void UserProfileRestoreWorker::accumulateFolderTotals(const FolderSelection& folder,
                                                      qint64& totalSize) {
    // Guard against negative or overflowing manifest-declared totals: these feed a
    // progress meter, so wrapping to a tiny value would badly mislead the user.
    // Saturate instead of wrapping, and ignore nonsensical negative counts.
    if (folder.size_bytes > 0) {
        if (totalSize > std::numeric_limits<qint64>::max() - folder.size_bytes) {
            totalSize = std::numeric_limits<qint64>::max();
        } else {
            totalSize += folder.size_bytes;
        }
    }
    if (folder.file_count > 0) {
        if (m_totalFilesToRestore > std::numeric_limits<int>::max() - folder.file_count) {
            m_totalFilesToRestore = std::numeric_limits<int>::max();
        } else {
            m_totalFilesToRestore += folder.file_count;
        }
    }
}

const BackupUserData* UserProfileRestoreWorker::findManifestUser(const QString& username) const {
    auto it = std::find_if(m_manifest.users.begin(),
                           m_manifest.users.end(),
                           [&username](const auto& user) { return user.username == username; });
    return (it != m_manifest.users.end()) ? &(*it) : nullptr;
}

void UserProfileRestoreWorker::updateProgress(qint64 bytesAdded) {
    Q_ASSERT(bytesAdded >= 0);
    m_bytesRestored += bytesAdded;

    if (m_filesRestored - m_lastProgressFileCount >= kRestoreProgressFileInterval ||
        m_bytesRestored - m_lastProgressByteCount >= kRestoreProgressByteInterval) {
        Q_EMIT fileProgress(m_filesRestored, m_totalFilesToRestore);

        m_lastProgressFileCount = m_filesRestored;
        m_lastProgressByteCount = m_bytesRestored;
    }
}

bool UserProfileRestoreWorker::hashFile(const QString& filePath, QByteArray& outDigest) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    if (!hasher.addData(&file)) {
        return false;
    }
    outDigest = hasher.result();
    return true;
}

bool UserProfileRestoreWorker::verifyFile(const QString& sourcePath, const QString& filePath) {
    Q_ASSERT(!sourcePath.isEmpty());
    Q_ASSERT(!filePath.isEmpty());
    QFileInfo fileInfo(filePath);

    if (!fileInfo.exists() || !fileInfo.isReadable()) {
        Q_EMIT logMessage(tr("Verification failed - file missing or unreadable: %1").arg(filePath),
                          true);
        return false;
    }

    // Re-hash the copied file and the source and require an exact match, so a
    // truncated or corrupted copy is caught post-write (not just readability).
    QByteArray sourceDigest;
    QByteArray destDigest;
    if (!hashFile(sourcePath, sourceDigest) || !hashFile(filePath, destDigest)) {
        Q_EMIT logMessage(tr("Verification failed - could not hash file: %1").arg(filePath), true);
        return false;
    }
    if (sourceDigest != destDigest) {
        Q_EMIT logMessage(tr("Verification failed - content mismatch: %1").arg(filePath), true);
        return false;
    }
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

    // Exhausted: return empty so the caller fails closed rather than overwriting.
    if (QFileInfo::exists(newPath)) {
        return {};
    }
    return newPath;
}

}  // namespace sak
