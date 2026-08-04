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

    /// @brief WiFi/Ethernet/AppData selections forwarded from the restore wizard's
    ///        network + app-data pages.
    struct RestoreSelections {
        QVector<WifiProfileInfo> wifi_profiles;
        QVector<EthernetConfigInfo> ethernet_configs;
        QVector<AppDataSourceInfo> app_data_sources;
    };

    /**
     * @brief Start restore operation
     * @param backupPath Path to backup directory
     * @param manifest Backup manifest with source data
     * @param mappings User mappings (source -> destination)
     * @param config Restore behavior configuration
     * @param selections WiFi/Ethernet/AppData selections (empty = none)
     */
    void startRestore(const QString& backupPath,
                      const BackupManifest& manifest,
                      const QVector<UserMapping>& mappings,
                      const RestoreConfig& config,
                      const RestoreSelections& selections = {});

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

    /// True if @p profileRelativePath falls under an app-data source in @p sources
    /// that the user unchecked (longest path-segment prefix wins). An empty
    /// @p sources never excludes. Pure decision, exposed for unit testing.
    [[nodiscard]] static bool isAppDataPathExcluded(const QString& profileRelativePath,
                                                    const QVector<AppDataSourceInfo>& sources);

    /// Resolve the System32-qualified netsh.exe path to defeat cwd search-order
    /// hijack. Returns empty (caller fails closed) when %SystemRoot% is unset.
    /// Exposed for unit testing.
    [[nodiscard]] static QString resolveSystem32Netsh();

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
    /// @brief Restore every selected folder of one user (extracted from restoreUser so the
    ///        post-restore integrity re-check keeps restoreUser under the length cap). Returns
    ///        false only on cancellation; per-folder failures increment m_filesErrored.
    bool restoreUserFolders(const UserMapping& mapping,
                            const BackupUserData& sourceUser,
                            const QString& sourcePath,
                            const QString& destProfilePath);
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
    /// @brief Accept a rename candidate into finalDestPath, or fail closed (count an
    ///        error, return false) when the candidate is empty (rename exhausted).
    bool applyRenameTarget(const QString& candidate,
                           const QFileInfo& destInfo,
                           QString& finalDestPath);
    bool applyPermissions(const QString& filePath, const QString& destinationUser);
    /// Win32 ownership assignment for AssignToDestination, with strip fallback if the
    /// SID cannot be resolved or ownership cannot be taken.
    bool assignOwnershipToUser(const QString& filePath, const QString& destinationUser);

    // Helpers
    bool validateBackup();
    bool verifyUserPayloadChecksums();
    /// @brief Verify one manifest user's payload tree against its sealed SHA-256 digest.
    ///        An absent digest is accepted only for a legacy backup when verification was NOT
    ///        requested (m_verify); with verify on, an absent or mismatched digest fails closed.
    ///        Reused post-restore so a source swapped after pre-restore validation is caught.
    bool verifyUserPayloadChecksum(const BackupUserData& user);
    /// @brief Find a user's data in the manifest by username
    const BackupUserData* findManifestUser(const QString& username) const;
    qint64 calculateTotalSize();
    /// @brief Add one folder's byte/file totals with saturating overflow guards
    void accumulateFolderTotals(const FolderSelection& folder, qint64& totalSize);
    void updateProgress(qint64 bytesAdded);
    /// @brief Verify a restored file against its source: existence, readability, and
    ///        an exact SHA-256 content match (fail closed on any mismatch).
    bool verifyFile(const QString& sourcePath, const QString& filePath);
    /// @brief SHA-256 a file's contents into @p outDigest; false if it cannot be read
    static bool hashFile(const QString& filePath, QByteArray& outDigest);
    QString resolveConflict(const QString& destPath);
    /// @param profileRelativeDir Path of @p destDir relative to the profile root
    ///        (e.g. "AppData/Local/Google"), used to honor the AppData page's
    ///        per-source selection: entries under an unchecked source are skipped.
    bool copyDirectory(const QString& sourceDir,
                       const QString& destDir,
                       const FolderSelection& folderConfig,
                       const QString& profileRelativeDir);
    /// Copy one directory entry (reparse-guarded recurse for a dir, conflict-resolved
    /// copy for a file). Errors are accumulated into m_filesErrored.
    void copyDirectoryEntry(const QFileInfo& entry,
                            const QString& destItem,
                            const FolderSelection& folderConfig,
                            const QString& entryRelative);

    // -- Network restore (WiFi / Ethernet) + AppData selection --------------

    /// Apply the selected WiFi + Ethernet settings after the file restore. A
    /// failure to apply any selected item counts as an error (fail closed).
    void applyNetworkSettings();
    /// Apply every selected WiFi profile; a failure is counted as an error.
    void applyWifiProfiles();
    /// Apply every selected Ethernet config; a failure is counted as an error.
    void applyEthernetConfigs();
    /// Re-import one WLAN profile via `netsh wlan add profile` from its stored XML.
    bool restoreWifiProfile(const WifiProfileInfo& profile);
    /// Apply one adapter's static-IP/DNS (or DHCP) config via `netsh interface ip`.
    bool restoreEthernetConfig(const EthernetConfigInfo& config);
    /// Return @p adapter to DHCP for address + DNS (idempotent: already-DHCP is
    /// success, verified structurally via the IP Helper API).
    bool restoreEthernetDhcp(const QString& adapter, const QString& netsh);
    /// Apply @p config as a static address + DNS on its adapter.
    bool restoreEthernetStatic(const EthernetConfigInfo& config, const QString& netsh);
    /// Run one netsh invocation, logging its stderr on failure. Returns true only on
    /// a clean exit-0 completion.
    bool runNetshLogged(const QString& netsh, const QStringList& args, const QString& adapter);

    // Data
    QString m_backupPath;
    BackupManifest m_manifest;
    QVector<UserMapping> m_mappings;
    // Selections forwarded from the restore wizard's WiFi/Ethernet/AppData pages.
    QVector<WifiProfileInfo> m_wifiProfiles;
    QVector<EthernetConfigInfo> m_ethernetConfigs;
    QVector<AppDataSourceInfo> m_appDataSources;
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
