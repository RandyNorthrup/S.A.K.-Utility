// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/user_profile_types.h"

#include <QFileInfo>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>

namespace sak {

class SmartFileFilter;
class PermissionManager;

/**
 * @brief Background worker for restoring user profile backups
 *
 * Handles:
 * - User mapping (source -> destination)
 * - Merge modes (replace, merge, create new)
 * - Conflict resolution
 * - Permission assignment
 * - File verification
 */
class UserProfileRestoreWorker : public QThread {
    Q_OBJECT

public:
    explicit UserProfileRestoreWorker(QObject* parent = nullptr);
    ~UserProfileRestoreWorker() override;

    /// @brief Configuration for restore operation behavior
    struct RestoreConfig {
        ConflictResolution conflict_mode;
        PermissionMode perm_mode;
        bool verify;
        bool create_backup;  // Save a recoverable copy before overwriting an existing file
    };

    /**
     * @brief Start restore operation
     * @param backupPath Path to backup directory
     * @param manifest Backup manifest with source data
     * @param mappings User mappings (source -> destination)
     * @param config Restore behavior configuration
     */
    void startRestore(const QString& backupPath,
                      const BackupManifest& manifest,
                      const QVector<UserMapping>& mappings,
                      const RestoreConfig& config);

    /**
     * @brief Cancel the restore operation
     */
    void cancel();

    /// High-level permission action decided purely from mode + destination user.
    enum class PermissionAction {
        StripPermissions,
        PreserveOriginal,
        AssignOwnership
    };

    /// Pure decision (no Win32) for applyPermissions. AssignToDestination with a
    /// NON-EMPTY user assigns ownership; with an empty user it falls back to
    /// stripping. Exposed for unit testing (B7-21).
    [[nodiscard]] static PermissionAction resolvePermissionAction(PermissionMode mode,
                                                                  const QString& destinationUser);

    /// The username whose ownership a restore assigns: the explicit destination, or
    /// the source when none is given. Exposed for unit testing (B7-21).
    [[nodiscard]] static QString effectiveDestUser(const UserMapping& mapping);

    // isRunning() is intentionally NOT overridden -- QThread::isRunning() is the source of truth
    // (true from the moment start() returns), so the destructor and the second-start guard cannot
    // race the late member flag that run() used to set.

Q_SIGNALS:
    /**
     * @brief Overall progress update
     * @param currentUser Current user index (1-based)
     * @param totalUsers Total number of users to restore
     * @param bytesCopied Bytes restored so far
     * @param totalBytes Total bytes to restore
     */
    void overallProgress(int currentUser, int totalUsers, qint64 bytesCopied, qint64 totalBytes);

    /**
     * @brief File-level progress
     * @param currentFile Current file number
     * @param totalFiles Total files to restore
     */
    void fileProgress(int currentFile, int totalFiles);

    /**
     * @brief Status update with current operation
     * @param username Current user being restored
     * @param operation Current operation description
     */
    void statusUpdate(const QString& username, const QString& operation);

    /**
     * @brief Log message
     * @param message Message text
     * @param isWarning true if warning, false if info
     */
    void logMessage(const QString& message, bool isWarning);

    /**
     * @brief Restore completed
     * @param success true if successful, false if failed
     * @param message Summary message
     */
    void restoreComplete(bool success, const QString& message);

protected:
    void run() override;

private:
    // Core operations
    bool restoreUser(const UserMapping& mapping);
    /// @brief Resolve the destination profile directory based on merge mode
    bool resolveDestinationProfilePath(const UserMapping& mapping, QString& destProfilePath);
    bool resolveCreateNewUser(const UserMapping& mapping,
                              const QString& systemDrive,
                              QString& destProfilePath);
    bool resolveExistingUser(const UserMapping& mapping, QString& destProfilePath);
    bool restoreFolder(const FolderSelection& folder,
                       const QString& sourcePath,
                       const QString& destPath);
    bool copyFileWithConflictResolution(const QString& source, const QString& dest, qint64 size);
    /// @brief Resolve file conflict according to m_conflictMode
    /// @return true if copy should proceed, false if file was skipped
    bool resolveFileConflict(const QString& source,
                             const QFileInfo& destInfo,
                             qint64 size,
                             QString& finalDestPath);
    QString generateConflictRenamePath(const QFileInfo& destInfo);
    bool applyPermissions(const QString& filePath, const QString& destinationUser);
    /// Win32 ownership assignment for AssignToDestination, with strip fallback if the
    /// SID cannot be resolved or ownership cannot be taken.
    bool assignOwnershipToUser(const QString& filePath, const QString& destinationUser);

    // Helpers
    bool validateBackup();
    bool verifyUserPayloadChecksums();
    bool createRestoreStructure();
    /// @brief Create standard user profile subdirectories inside a destination dir
    void createStandardSubfolders(const QDir& destDir);
    /// @brief Find a user's data in the manifest by username
    const BackupUserData* findManifestUser(const QString& username) const;
    qint64 calculateTotalSize();
    void updateProgress(qint64 bytesAdded);
    bool verifyFile(const QString& filePath);
    QString resolveConflict(const QString& destPath);
    bool copyDirectory(const QString& sourceDir,
                       const QString& destDir,
                       const FolderSelection& folderConfig);

    // Data
    QString m_backupPath;
    BackupManifest m_manifest;
    QVector<UserMapping> m_mappings;
    /// Effective destination username of the mapping currently being restored, set
    /// at the top of restoreUser(). Passed to applyPermissions so
    /// PermissionMode::AssignToDestination actually assigns ownership to the target
    /// user instead of always falling back to stripping permissions (B7-21). Safe as
    /// a member: restore runs one mapping at a time on the worker thread.
    QString m_currentDestUser;
    ConflictResolution m_conflictMode{ConflictResolution::SkipDuplicate};
    PermissionMode m_permissionMode{PermissionMode::StripAll};
    bool m_verify{false};
    bool m_createBackup{false};

    // Progress tracking
    std::atomic<bool> m_cancelled{false};
    QMutex m_mutex;

    qint64 m_totalBytesToRestore{0};
    qint64 m_bytesRestored{0};
    int m_totalFilesToRestore{0};
    int m_filesRestored{0};
    int m_filesSkipped{0};
    int m_filesErrored{0};
    // Progress-throttle watermarks as members, not function-static locals, so two
    // restore instances never share/race the throttle state (B7-34).
    int m_lastProgressFileCount{0};
    qint64 m_lastProgressByteCount{0};

    // Instances
    SmartFileFilter* m_fileFilter;
    PermissionManager* m_permissionManager;
};

}  // namespace sak
