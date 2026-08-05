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

namespace {
// kBackupWorkerMaxCompressionLevel was removed with the assert that was its only
// use. Range-checking a value this worker deliberately ignores (it copies files
// verbatim and says so at run()) validated nothing.
constexpr int kBackupSizeDisplayPrecision = 2;
constexpr int kProgressEmitFileInterval = 100;
constexpr qint64 kProgressEmitByteInterval = kProgressEmitFileInterval * kBytesPerMB;
constexpr double kDiskSpaceSafetyMultiplier = 1.1;
constexpr int kDiskSpaceDisplayPrecision = 1;

// True if the path is an NTFS reparse point (junction/symlink/mount point).
bool isReparsePoint(const QString& path) {
#ifdef Q_OS_WIN
    const DWORD attrs = GetFileAttributesW(path.toStdWString().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return QFileInfo(path).isSymLink();
#endif
}
}  // namespace

UserProfileBackupWorker::UserProfileBackupWorker(QObject* parent)
    : QThread(parent)
    , m_fileFilter(new SmartFileFilter())
    , m_permissionManager(new PermissionManager()) {}

UserProfileBackupWorker::~UserProfileBackupWorker() {
    if (isRunning()) {
        cancel();
        // Never let ~QThread run on a live thread: if an in-flight copy of a
        // large/stalled file outlasts the grace period, block until it finishes.
        if (!wait(kTimeoutThreadTerminateMs)) {
            wait();
        }
    }
    delete m_fileFilter;
    delete m_permissionManager;
}

void UserProfileBackupWorker::startBackup(const BackupManifest& manifest,
                                          const QVector<UserProfile>& users,
                                          const QString& destinationPath,
                                          const SmartFilter& smartFilter,
                                          const BackupOptions& options) {
    // No asserts on these. An empty user list is a backup of nothing, which completes
    // with an empty manifest, and an empty destination fails the path validation below.
    // Asserting either would abort a Debug build on input that Release accepts, which is
    // what made the mirror-image restore worker unable to run its own tests in Debug.
    if (isRunning()) {
        Q_EMIT logMessage(tr("Backup already in progress"), true);
        return;
    }

    QMutexLocker locker(&m_mutex);

    m_manifest = manifest;
    m_users = users;
    m_destinationPath = destinationPath;
    m_smartFilter = smartFilter;
    m_permissionMode = options.permission_mode;

    m_cancelled = false;
    m_bytesCopied = 0;
    m_filesCopied = 0;
    m_filesSkipped = 0;
    m_filesErrored = 0;
    m_filesElevationSkipped = 0;
    m_lastProgressFileCount = 0;
    m_lastProgressByteCount = 0;
    m_currentUserProfile.clear();

    // Apply filter settings
    m_fileFilter->setRules(smartFilter);
    // An exclude pattern that fails to compile is dropped, so files the user meant
    // to exclude WILL be backed up (fail-open). Surface it rather than silently
    // backing up what they asked to skip (B8-23).
    const QStringList invalidPatterns = m_fileFilter->invalidPatterns();
    if (!invalidPatterns.isEmpty()) {
        Q_EMIT logMessage(tr("Ignoring %1 invalid exclusion pattern(s); matching files will NOT "
                             "be excluded from the backup: %2")
                              .arg(invalidPatterns.size())
                              .arg(invalidPatterns.join(QStringLiteral("; "))),
                          true);
    }

    start();
}

void UserProfileBackupWorker::cancel() {
    m_cancelled = true;
    Q_EMIT logMessage(tr("Canceling backup..."), false);
}

void UserProfileBackupWorker::run() {
    // See startBackup: an empty user list backs up nothing and is not an abort.
    Q_EMIT logMessage(tr("=== Backup Started ==="), false);
    Q_EMIT logMessage(tr("Destination: %1").arg(m_destinationPath), false);
    Q_EMIT logMessage(tr("Users to backup: %1").arg(m_users.size()), false);

    // The encryption refusal and the "compression is not applied" note that used to sit
    // here are gone with the wizard controls that caused them: this worker copies files
    // verbatim, and nothing now claims otherwise.

    // Validate inputs
    if (!validateSourcePaths()) {
        Q_EMIT backupComplete(false, tr("Invalid source paths"), m_manifest);
        return;
    }

    // Calculate the total size first so the disk-space check has a real
    // requirement to compare against (an empty total would always pass).
    Q_EMIT logMessage(tr("Calculating total size..."), false);
    m_totalBytesToCopy = calculateTotalSize();
    Q_EMIT logMessage(
        tr("Total estimated size: %1 GB")
            .arg(m_totalBytesToCopy / sak::kBytesPerGBf, 0, 'f', kBackupSizeDisplayPrecision),
        false);

    // Check disk space
    if (!checkDiskSpace()) {
        Q_EMIT backupComplete(false, tr("Insufficient disk space"), m_manifest);
        return;
    }

    // Create backup directory structure
    if (!createBackupStructure()) {
        Q_EMIT backupComplete(false, tr("Failed to create backup structure"), m_manifest);
        return;
    }

    // Backup each user
    backupAllUsers();

    // A cancel may have been requested mid-copy; report failure and never fall
    // through to the success summary (exactly one terminal signal is emitted).
    if (m_cancelled) {
        Q_EMIT backupComplete(false, tr("Backup cancelled"), m_manifest);
        return;
    }

    // Save manifest and emit summary
    emitBackupSummary();
}

void UserProfileBackupWorker::backupAllUsers() {
    // An empty list simply iterates zero times; see startBackup.
    int userIndex = 0;
    for (const auto& user : m_users) {
        if (m_cancelled) {
            // run() owns the single terminal backupComplete signal; just stop.
            Q_EMIT logMessage(tr("Backup cancelled by user"), true);
            return;
        }

        if (!user.is_selected) {
            continue;
        }

        // The username becomes a backup subdirectory. Reject a crafted name ("..\\..\\Windows",
        // "C:\\evil") so it can never escape the destination root -- fail closed (counts as an
        // error so the backup is not reported as a clean success).
        if (!isSafePathSegment(user.username)) {
            Q_EMIT logMessage(tr("Skipping user with unsafe name: %1").arg(user.username), true);
            ++m_filesErrored;
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
    const bool manifestSaved = saveManifest();
    if (!manifestSaved) {
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
    // Report success only when the manifest was written AND nothing was left out:
    // any hard error OR an elevation-required skip means the backup is incomplete
    // (some selected data was not captured), so it must not read as a clean success
    // (B7-20). Intentional filter skips (m_filesSkipped: caches, *.tmp) do not count.
    const bool success = (m_filesErrored == 0) && (m_filesElevationSkipped == 0) && manifestSaved;
    Q_EMIT backupComplete(success, summary, m_manifest);
}

bool UserProfileBackupWorker::backupUser(const UserProfile& user, const QString& userBackupPath) {
    // An empty userBackupPath fails the directory creation below and is reported.
    // Filter exclusion rules are keyed on THIS user's profile path (B7-34).
    m_currentUserProfile = user.profile_path;
    // Create user backup directory
    QDir dir;
    if (!dir.mkpath(userBackupPath)) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(userBackupPath), true);
        // A user whose backup root cannot be created backed up nothing; count it as
        // an error so the run is not reported as a clean success (B7-20).
        ++m_filesErrored;
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

        // relative_path is joined onto BOTH the source profile and the destination roots; a "../"
        // in it would read outside the profile and write outside the backup. Reject traversal.
        if (!isSafeRelativePath(folder.relative_path)) {
            Q_EMIT logMessage(tr("Skipping folder with unsafe path: %1").arg(folder.relative_path),
                              true);
            ++m_filesErrored;
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
    // An empty sourcePath fails the exists() check below; see startBackup.
    QFileInfo sourceInfo(sourcePath);

    if (!sourceInfo.exists()) {
        Q_EMIT logMessage(tr("Source does not exist: %1").arg(sourcePath), true);
        // A selected folder that is missing at backup time is an error, not a silent
        // success: the resulting backup omits data the user asked to keep (B7-20).
        ++m_filesErrored;
        return false;
    }

    // A reparse point (symlink/junction) must never be followed: a directory one loops or escapes
    // the profile, and a FILE one would copy the content of whatever it targets (e.g. a symlink to
    // a privileged system file) into the backup. Skip both, not just directories.
    if (isReparsePoint(sourcePath)) {
        Q_EMIT logMessage(tr("Skipping reparse point: %1").arg(sourcePath), false);
        m_filesSkipped++;
        return true;
    }

    if (sourceInfo.isDir()) {
        return copyDirectory(sourcePath, destPath, folder);
    }
    if (sourceInfo.isFile()) {
        return copyFileWithFiltering(sourcePath, destPath, sourceInfo.size());
    }

    // Exists but is neither a regular file nor a directory (device/pipe/etc.): it
    // could not be backed up, so record it rather than returning a silent failure.
    Q_EMIT logMessage(tr("Unsupported source type: %1").arg(sourcePath), true);
    ++m_filesErrored;
    return false;
}

bool UserProfileBackupWorker::copyDirectory(const QString& sourceDir,
                                            const QString& destDir,
                                            const FolderSelection& folderConfig) {
    // Empty directories fail the checks below; see startBackup.
    // Check if folder should be excluded
    QFileInfo sourceDirInfo(sourceDir);
    const QString& currentUserProfile = m_currentUserProfile;

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
        // Counted here (once) as the leaf failure; callers only log/propagate so the
        // error is not double-counted (B7-20).
        ++m_filesErrored;
        return false;
    }

    // Iterate through directory contents
    QDirIterator it(sourceDir,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    while (it.hasNext()) {
        if (m_cancelled) {
            return false;
        }
        copyDirectoryEntry(it.next(), destDir, folderConfig);
    }

    return true;
}

void UserProfileBackupWorker::copyDirectoryEntry(const QString& sourceItem,
                                                 const QString& destDir,
                                                 const FolderSelection& folderConfig) {
    QFileInfo itemInfo(sourceItem);
    QString destItem = destDir + "/" + itemInfo.fileName();

    // Never follow a reparse point (junction/symlink), whether it is a directory OR a file: a
    // directory one can loop back into the profile or point outside it (foreign/privileged data),
    // and a FILE symlink would pull the content of its target (e.g. a system file) into the backup.
    if (isReparsePoint(sourceItem)) {
        Q_EMIT logMessage(tr("Skipping reparse point: %1").arg(sourceItem), false);
        return;
    }

    if (itemInfo.isDir()) {
        if (!copyDirectory(sourceItem, destItem, folderConfig)) {
            Q_EMIT logMessage(tr("Warning: Failed to copy directory: %1").arg(sourceItem), true);
        }
        return;
    }

    if (itemInfo.isFile()) {
        copyFileWithFiltering(sourceItem, destItem, itemInfo.size());
    }
}

bool UserProfileBackupWorker::copyFileWithFiltering(const QString& sourcePath,
                                                    const QString& destPath,
                                                    qint64 fileSize) {
    // Empty paths fail the file operations below; see startBackup. fileSize stays
    // asserted because it is an invariant this class guarantees: it comes from
    // QFileInfo::size(), which reports 0 - never a negative - for a file it
    // cannot stat.
    Q_ASSERT_X(fileSize >= 0, "copyFileWithFiltering", "fileSize must be non-negative");
    QFileInfo sourceInfo(sourcePath);
    const QString& currentUserProfile = m_currentUserProfile;

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
        // The file cannot be written without its parent dir; count it (B7-20).
        Q_EMIT logMessage(tr("Failed to create directory for: %1").arg(destPath), true);
        ++m_filesErrored;
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
    // m_permissionManager stays asserted: it is owned and constructed by this
    // class, so a null there means the object is corrupt. filePath is not
    // asserted - an empty one fails inside the permission manager and is
    // reported.
    Q_ASSERT(m_permissionManager);
    switch (m_permissionMode) {
    case PermissionMode::StripAll:
        return m_permissionManager->stripPermissions(filePath);

    case PermissionMode::PreserveOriginal:
        // Do nothing, keep original permissions
        return true;

    case PermissionMode::AssignToDestination:
        // This would be done during restore, not backup
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
        // Digest the user's backed-up payload so restore can detect corruption or
        // tampering of the stored files (B7-13).
        userData.checksum_sha256 =
            BackupManifest::hashDirectoryTree(m_destinationPath + "/" + user.username);

        m_manifest.users.append(userData);
    }

    // Seal the manifest with a self-excluding digest over its final content (users
    // + their checksums must already be set) so a modified manifest.json is caught
    // at restore.
    m_manifest.manifest_checksum.clear();
    m_manifest.manifest_checksum = m_manifest.computeManifestChecksum();

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
    // Invariant: the sole caller (copyFileWithFiltering) forwards its fileSize, which both of
    // its call sites take from QFileInfo::size() -- 0, never negative, for an unstattable file.
    Q_ASSERT(bytesAdded >= 0);
    m_bytesCopied += bytesAdded;

    if (m_filesCopied - m_lastProgressFileCount >= kProgressEmitFileInterval ||
        m_bytesCopied - m_lastProgressByteCount >= kProgressEmitByteInterval) {
        int currentUser = 0, totalUsers = 0;
        countSelectedUsers(currentUser, totalUsers);

        Q_EMIT overallProgress(currentUser, totalUsers, m_bytesCopied, m_totalBytesToCopy);
        Q_EMIT fileProgress(m_filesCopied, m_totalFilesToCopy);

        m_lastProgressFileCount = m_filesCopied;
        m_lastProgressByteCount = m_bytesCopied;
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

bool UserProfileBackupWorker::isSafePathSegment(const QString& segment) {
    if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral("..")) {
        return false;
    }
    static const QString kForbidden = QStringLiteral("/\\:*?\"<>|");
    for (const QChar c : segment) {
        if (kForbidden.contains(c)) {
            return false;
        }
    }
    return true;
}

bool UserProfileBackupWorker::isSafeRelativePath(const QString& rel) {
    if (rel.isEmpty() || QDir::isAbsolutePath(rel) || rel.contains(QLatin1Char(':'))) {
        return false;  // absolute (C:\ or /x) or drive-relative (C:foo)
    }
    if (rel.startsWith(QLatin1Char('/')) || rel.startsWith(QLatin1Char('\\'))) {
        return false;  // leading slash / UNC
    }
    const QStringList parts = QDir::fromNativeSeparators(rel).split(QLatin1Char('/'),
                                                                    Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) {
            return false;  // traversal component
        }
    }
    return true;
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

    requiredBytes = qRound64(static_cast<double>(requiredBytes) * kDiskSpaceSafetyMultiplier);

    if (availableBytes < requiredBytes) {
        double availableGB = availableBytes / sak::kBytesPerGBf;
        double requiredGB = requiredBytes / sak::kBytesPerGBf;

        Q_EMIT logMessage(tr("Insufficient disk space. Available: %1 GB, Required: %2 GB")
                              .arg(availableGB, 0, 'f', kDiskSpaceDisplayPrecision)
                              .arg(requiredGB, 0, 'f', kDiskSpaceDisplayPrecision),
                          true);
        return false;
    }

    Q_EMIT logMessage(tr("Disk space check passed"), false);
    return true;
}

}  // namespace sak
