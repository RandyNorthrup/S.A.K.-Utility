// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/permission_manager.h"
#include "sak/smart_file_filter.h"
#include "sak/user_profile_types.h"

#include <QMutex>
#include <QThread>

#include <atomic>

namespace sak {

/**
 * @brief Background worker for backing up user profiles
 *
 * Copies selected user profile files/folders to backup destination
 * with smart filtering, permission handling, and progress reporting.
 *
 * This worker copies files verbatim. It has never compressed or encrypted
 * anything, so the compression_level / encrypt / password options were removed
 * along with the wizard controls that fed them: the wizard collected a password
 * twice, logged "Encryption: Enabled (AES-256)", and then the run aborted with
 * "Encrypted backup is not supported", while the compression box defaulted to
 * Balanced and produced an uncompressed copy on every default run. Advertising
 * either one is worse than not offering it.
 *
 * UserDataManager DOES implement both for application-data backups. Bringing
 * them here means an archive format the restore side can read, a password
 * prompt in the restore wizard, and extract-to-temp-then-wipe so selective
 * folder restore keeps working - a real feature, not a checkbox. See R5-G19-1.
 */
class UserProfileBackupWorker : public QThread {
    Q_OBJECT

public:
    explicit UserProfileBackupWorker(QObject* parent = nullptr);
    ~UserProfileBackupWorker() override;

    /// @brief Options controlling backup behavior
    struct BackupOptions {
        PermissionMode permission_mode{PermissionMode::StripAll};
    };

    /**
     * @brief Start backup operation
     * @param manifest Backup configuration and metadata
     * @param users List of users to backup with folder selections
     * @param destinationPath Where to save the backup
     * @param smartFilter Filter rules for excluding files
     * @param options Backup behavior options (permissions, compression, encryption)
     */
    void startBackup(const BackupManifest& manifest,
                     const QVector<UserProfile>& users,
                     const QString& destinationPath,
                     const SmartFilter& smartFilter,
                     const BackupOptions& options);

    /**
     * @brief Cancel the backup operation
     */
    void cancel();

    // isRunning() is intentionally NOT overridden -- QThread::isRunning() is the source of truth.
    // It is true from the moment start() returns (Qt sets the running state inside start(), before
    // run() begins), closing the window where a late-set member flag let the destructor tear down
    // a live thread or a second startBackup() slip past the guard.

    /// @brief Check if current process can read the given path
    [[nodiscard]] static bool canReadPath(const QString& path);

    /// @brief True if @p segment is a single safe path component: non-empty, not "."/".." and free
    /// of separators, drive colons, and wildcards. Validates an untrusted username before it
    /// becomes a backup subdirectory (a name like "..\\..\\Windows" must never escape the root).
    [[nodiscard]] static bool isSafePathSegment(const QString& segment);

    /// @brief True if @p rel is a safe RELATIVE path: non-empty, not absolute (no drive letter, no
    /// leading slash/UNC), and containing no "." or ".." component. Validates an untrusted
    /// folder.relative_path before it is joined onto the source profile and destination roots.
    [[nodiscard]] static bool isSafeRelativePath(const QString& rel);

Q_SIGNALS:
    /**
     * @brief Overall backup progress
     * @param currentUser Index of current user being backed up
     * @param totalUsers Total number of users
     * @param bytesCopied Total bytes copied so far
     * @param totalBytes Estimated total bytes to copy
     */
    void overallProgress(int currentUser, int totalUsers, qint64 bytesCopied, qint64 totalBytes);

    /**
     * @brief Current file/folder progress
     * @param currentFile Index of current file
     * @param totalFiles Total files in current operation
     */
    void fileProgress(int currentFile, int totalFiles);

    /**
     * @brief Current operation status
     * @param username Current user being backed up
     * @param operation Description of current operation
     */
    void statusUpdate(const QString& username, const QString& operation);

    /**
     * @brief Log message for detailed tracking
     * @param message Log message
     * @param isWarning true if warning, false for info
     */
    void logMessage(const QString& message, bool isWarning = false);

    /**
     * @brief A file or folder was skipped because it requires elevation
     * @param path The inaccessible path
     * @param owner Owner of the file/folder (if determinable)
     */
    void elevationSkipped(const QString& path, const QString& owner);

    /**
     * @brief Backup completed (success or failure)
     * @param success true if completed successfully
     * @param message Summary message
     * @param manifest Final manifest with actual stats
     */
    void backupComplete(bool success, const QString& message, const BackupManifest& manifest);

protected:
    void run() override;

private:
    // Core backup operations
    /// @brief Run all user backup iterations
    void backupAllUsers();
    /// @brief Compose and emit the final backup summary
    void emitBackupSummary();
    bool backupUser(const UserProfile& user, const QString& userBackupPath);
    bool backupFolder(const FolderSelection& folder,
                      const QString& sourcePath,
                      const QString& destPath);
    bool copyFileWithFiltering(const QString& sourcePath, const QString& destPath, qint64 fileSize);
    bool applyPermissions(const QString& filePath);

    // Helper functions
    bool createBackupStructure();
    bool saveManifest();
    qint64 calculateTotalSize();
    /// @brief Sum folder sizes for a single selected user
    qint64 accumulateUserFolderSizes(const UserProfile& user);
    void updateProgress(qint64 bytesAdded);
    /// @brief Count selected users for progress tracking
    void countSelectedUsers(int& currentUser, int& totalUsers) const;

    // Directory operations
    bool copyDirectory(const QString& sourceDir,
                       const QString& destDir,
                       const FolderSelection& folderConfig);
    void copyDirectoryEntry(const QString& sourceItem,
                            const QString& destDir,
                            const FolderSelection& folderConfig);
    bool createDirectory(const QString& path);

    // Validation
    bool validateSourcePaths();
    bool checkDiskSpace();

    // Configuration
    BackupManifest m_manifest;
    QVector<UserProfile> m_users;
    QString m_destinationPath;
    SmartFilter m_smartFilter;
    PermissionMode m_permissionMode{PermissionMode::StripAll};

    // Progress tracking
    std::atomic<bool> m_cancelled{false};
    QMutex m_mutex;

    qint64 m_totalBytesToCopy{0};
    qint64 m_bytesCopied{0};
    int m_totalFilesToCopy{0};
    int m_filesCopied{0};
    int m_filesSkipped{0};
    int m_filesErrored{0};
    int m_filesElevationSkipped{0};
    // Progress-throttle watermarks. Members, not function-static locals, so two
    // worker instances (or a reused one) never share/race the throttle state (B7-34).
    int m_lastProgressFileCount{0};
    qint64 m_lastProgressByteCount{0};
    // profile_path of the user currently being backed up, so smart-filter exclusion
    // rules key on the RIGHT user's profile instead of always m_users[0] (B7-34).
    QString m_currentUserProfile;

    // Instances for operations
    SmartFileFilter* m_fileFilter{nullptr};
    PermissionManager* m_permissionManager{nullptr};
};

}  // namespace sak
