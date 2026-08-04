// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QCryptographicHash>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

#include <initializer_list>
#include <optional>
#include <vector>

class QDir;

namespace sak {

inline constexpr int kDefaultBackupCompressionLevel = 6;

/**
 * @brief UserDataManager - Backup and restore application user data
 *
 * Manages backup and restore operations for application user data folders.
 * Supports selective backup, compression, integrity verification, and
 * automatic detection of common app data locations.
 *
 * Key Features:
 * - Auto-detect common app data paths (AppData, ProgramData, Documents)
 * - Selective file/folder backup
 * - Compression (zip format)
 * - Checksums for integrity verification
 * - Incremental backup support
 * - Restore with conflict resolution
 */
class UserDataManager : public QObject {
    Q_OBJECT

public:
    explicit UserDataManager(QObject* parent = nullptr);
    ~UserDataManager() = default;

    /**
     * @brief Backup entry for a single app's data
     */
    struct BackupEntry {
        QString app_name;
        QString app_version;
        QStringList source_paths;         // Source directories/files
        QString backup_path;              // Archive file path
        QDateTime backup_date;
        qint64 total_size_bytes{0};       // Total bytes
        qint64 compressed_size_bytes{0};  // Archive size
        QString checksum;                 // SHA256 of archive
        bool encrypted{false};            // Future: encryption support
        QStringList excluded_patterns;    // Excluded files (*.log, *.tmp)
    };

    /**
     * @brief Backup configuration
     */
    struct BackupConfig {
        bool compress = true;           // Create zip archive
        bool verify_checksum = true;    // Verify after backup
        bool include_registry = false;  // Future: backup registry keys
        QStringList exclude_patterns = {// Default exclusions
                                        "*.log",
                                        "*.tmp",
                                        "cache/*",
                                        "temp/*"};
        int compression_level = kDefaultBackupCompressionLevel;  // 0-9 (0=none, 9=max)
        bool encrypt = false;                                    // Enable AES-256 encryption
        QString password;                                        // Encryption password
    };

    /**
     * @brief Restore configuration
     */
    struct RestoreConfig {
        bool verify_checksum = true;      // Verify before restore
        bool create_backup = true;        // Backup existing data
        bool overwrite_existing = false;  // Overwrite without prompt
        bool restore_timestamps = true;   // Preserve file dates
        QString password;                 // Decryption password (if encrypted)
    };

    /**
     * @brief Data location patterns for common apps
     */
    struct DataLocation {
        QString pattern;      // Name pattern (e.g., "Google Chrome")
        QStringList paths;    // Possible data paths
        QString description;  // User-friendly description
    };

    // Backup operations
    std::optional<BackupEntry> backupAppData(const QString& app_name,
                                             const QStringList& source_paths,
                                             const QString& backup_dir,
                                             const BackupConfig& config = BackupConfig());

    bool backupMultipleApps(const QStringList& app_names,
                            const QString& backup_dir,
                            const BackupConfig& config = BackupConfig());

    // Restore operations
    bool restoreAppData(const QString& backup_path,
                        const QString& restore_dir,
                        const RestoreConfig& config = RestoreConfig());

    bool restoreMultipleApps(const QStringList& backup_paths,
                             const QString& restore_dir,
                             const RestoreConfig& config = RestoreConfig());

    // Data discovery
    QStringList discoverAppDataPaths(const QString& app_name) const;
    std::vector<DataLocation> getCommonDataLocations() const;
    QStringList scanForAppData(const QString& app_name) const;

    // Backup management
    std::vector<BackupEntry> listBackups(const QString& backup_dir) const;
    BackupEntry getBackupInfo(const QString& backup_path) const;
    bool deleteBackup(const QString& backup_path);
    bool verifyBackup(const QString& backup_path);

    /// @brief Decide whether the backup at @p backup_path may be recursively deleted.
    ///
    /// deleteBackup recursively erases whatever directory it is handed, so a confused or
    /// prompt-injected caller could weaponize it to wipe an arbitrary tree (C:\\Windows, a
    /// user's Documents, a junction redirected elsewhere). This pure seam is the fail-closed
    /// gate: deletion is REFUSED unless (a) @p backup_path clears the shared root/UNC/device/
    /// reparse/system screens (filePathDeletionRefusal) AND (b) a sidecar metadata file exists
    /// whose recorded backup_path identifies this exact object (@p recorded_backup_path).
    /// std::nullopt means no valid sidecar was found -- an unmanaged directory the manager never
    /// created, which is never deleted. Returns an empty string when deletion is allowed, else a
    /// human-readable refusal reason. Pure (no I/O) so it is unit-testable in isolation.
    static QString backupDeletionRefusal(const QString& backup_path,
                                         const std::optional<QString>& recorded_backup_path);

    /// @brief True only when every supplied path is non-empty. Release-effective replacement for
    ///        the old Q_ASSERT_X entry guards (a no-op in release, which let empty paths run
    ///        CWD-relative). Pure (no I/O), unit-testable in isolation.
    static bool allPathsPresent(std::initializer_list<QString> paths);

    /// @brief True when an attacker-supplied encrypted archive's size is within the bound we will
    ///        read into memory (0 <= size <= 4 GiB). Pure seam for the fail-closed pre-read cap in
    ///        decryptArchiveToTempFile (guards OOM before the zip-bomb preflight, which only runs
    ///        on the decrypted .zip).
    static bool encryptedArchiveSizeOk(qint64 size);

    /// @brief Atomically replace @p target with @p tmp by renaming the original aside first;
    ///        rolls the original back and cleans up @p tmp if the swap fails (no data-loss window).
    static bool atomicReplaceFile(const QString& tmp, const QString& target);

    // Utilities
    qint64 calculateSize(const QStringList& paths) const;
    QString generateChecksum(const QString& file_path) const;
    bool compareChecksums(const QString& file1, const QString& file2) const;

Q_SIGNALS:
    /**
     * @brief Emitted when backup/restore starts
     */
    void operationStarted(const QString& app_name, const QString& operation);

    /**
     * @brief Emitted for progress updates
     * @param current Current file/bytes
     * @param total Total files/bytes
     * @param message Status message
     */
    void progressUpdate(qint64 current, qint64 total, const QString& message);

    /**
     * @brief Emitted when operation completes
     */
    void operationCompleted(const QString& app_name, bool success, const QString& message);

    /**
     * @brief Emitted for errors
     */
    void operationError(const QString& app_name, const QString& error);

private:
    /**
     * @brief Create zip archive from paths
     */
    bool createArchive(const QStringList& source_paths,
                       const QString& archive_path,
                       const BackupConfig& config);

    /// @brief Build the (unencrypted) archive payload, staging a filtered copy first
    ///        when exclude_patterns are set so excluded files never enter the zip.
    bool buildArchivePayload(const QStringList& source_paths,
                             const QString& archive_path,
                             const BackupConfig& config);

    /// @brief Run Compress-Archive over the given source paths (no filtering, no encryption)
    static bool compressSourcePaths(const QStringList& source_paths,
                                    const QString& archive_path,
                                    int compression_level);

    /// @brief Refuse an archive whose entry count or decompressed size exceeds the
    ///        zip-bomb caps, enumerated before any extraction writes to disk.
    static bool archiveWithinLimits(const QString& archive_path);

    /// @brief Map BackupConfig compression_level to PowerShell CompressionLevel name
    static QString mapCompressionLevel(int level);
    /// @brief Encrypt an existing archive file in-place using AES-256
    bool encryptArchiveInPlace(const QString& archive_path, const BackupConfig& config);

    /**
     * @brief Extract zip archive
     */
    bool extractArchive(const QString& archive_path,
                        const QString& destination,
                        const RestoreConfig& config);

    /// @brief Decrypt archive to a temporary file for extraction
    /// @return Path to temporary decrypted file, or empty string on failure
    QString decryptArchiveToTempFile(const QString& archive_path, const QString& password);

    /**
     * @brief Check if path matches exclusion patterns
     */
    bool isExcluded(const QString& path, const QStringList& patterns) const;

    /**
     * @brief Get standard app data locations
     */
    QStringList getStandardDataPaths() const;

    /// @brief What to do when a destination file already exists during a copy.
    /// Fail = collision is an error (backup creation: two distinct sources must never merge into
    /// one name). Skip = leave the existing file (restore with overwrite_existing=false).
    /// Overwrite = replace it (restore with overwrite_existing=true).
    enum class ExistingFilePolicy {
        Fail,
        Skip,
        Overwrite
    };

    /**
     * @brief Recursively copy directory
     * @param policy How to treat a destination file that already exists (see ExistingFilePolicy).
     *        Defaults to Fail so backup-creation collisions stay fail-closed.
     */
    bool copyDirectory(const QString& source,
                       const QString& destination,
                       const QStringList& exclude_patterns,
                       ExistingFilePolicy policy = ExistingFilePolicy::Fail);

    /// @brief Copy the plain (non-directory) files of one dir; fail closed on error.
    /// @param policy How an already-existing destination file is handled (see ExistingFilePolicy).
    bool copyPlainFiles(const QDir& source_dir,
                        const QDir& dest_dir,
                        const QStringList& exclude_patterns,
                        ExistingFilePolicy policy);

    /// @brief Overwrite an existing destination file without a data-loss window:
    ///        copy to a sibling temp first, then atomically replace, so a copy
    ///        failure never destroys the original.
    static bool overwriteFile(const QString& source_file, const QString& dest_file);


    /// @brief Copy all source paths into a single destination directory
    bool copySourcesToDest(const QStringList& source_paths,
                           const QString& dest_dir,
                           const QStringList& exclude_patterns);

    /**
     * @brief Calculate SHA256 checksum
     */
    QString calculateSHA256(const QString& file_path) const;

    /**
     * @brief Write backup metadata file
     */
    bool writeMetadata(const BackupEntry& entry, const QString& metadata_path);

    /**
     * @brief Read backup metadata file
     */
    std::optional<BackupEntry> readMetadata(const QString& metadata_path) const;

    /// @brief Build a BackupEntry describing a completed backup (no I/O)
    BackupEntry buildBackupResult(const QString& app_name,
                                  const QStringList& source_paths,
                                  const QString& archive_path,
                                  qint64 total_size,
                                  const BackupConfig& config);

    /// @brief Validate a backup request; emits operationError and returns false on reject
    bool validateBackupRequest(const QString& app_name,
                               const QStringList& source_paths,
                               const QString& backup_dir,
                               const BackupConfig& config);

    /// @brief Write the backup payload (archive or copy tree); emits error on failure
    bool writeBackupPayload(const QString& app_name,
                            const QStringList& source_paths,
                            const QString& backup_path,
                            const BackupConfig& config);

    /// @brief Resolve a collision-free backup path (payload + ".json" sidecar)
    static QString uniqueBackupPath(const QString& backup_dir,
                                    const QString& base_name,
                                    bool compress);

    /// @brief Remove a partially written backup payload (archive file or dir)
    static void removeBackupPayload(const QString& backup_path, bool compress);

    /// @brief Restore a backup payload (zip archive or directory) into restore_dir
    bool restorePayload(const QString& backup_path,
                        const QString& restore_dir,
                        const BackupEntry& entry,
                        const RestoreConfig& config);

    /// @brief Integrity-gate a restore. When verification is requested a compressed
    ///        archive MUST carry a checksum (a tampered/emptied sidecar cannot silently
    ///        disable it); returns false and emits operationError when it cannot verify.
    bool verifyRestoreIntegrity(const QString& backup_path,
                                const BackupEntry& entry,
                                const RestoreConfig& config);
};

}  // namespace sak
