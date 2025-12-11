#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QCryptographicHash>
#include <vector>
#include <optional>

namespace sak {

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
        QStringList source_paths;      // Source directories/files
        QString backup_path;           // Archive file path
        QDateTime backup_date;
        qint64 total_size;             // Total bytes
        qint64 compressed_size;        // Archive size
        QString checksum;              // SHA256 of archive
        bool encrypted;                // Future: encryption support
        QStringList excluded_patterns; // Excluded files (*.log, *.tmp)
    };

    /**
     * @brief Backup configuration
     */
    struct BackupConfig {
        bool compress = true;              // Create zip archive
        bool verify_checksum = true;       // Verify after backup
        bool include_registry = false;     // Future: backup registry keys
        QStringList exclude_patterns = {   // Default exclusions
            "*.log", "*.tmp", "cache/*", "temp/*"
        };
        int compression_level = 6;         // 0-9 (0=none, 9=max)
        bool encrypt = false;              // Enable AES-256 encryption
        QString password;                  // Encryption password
    };

    /**
     * @brief Restore configuration
     */
    struct RestoreConfig {
        bool verify_checksum = true;       // Verify before restore
        bool create_backup = true;         // Backup existing data
        bool overwrite_existing = false;   // Overwrite without prompt
        bool restore_timestamps = true;    // Preserve file dates
        QString password;                  // Decryption password (if encrypted)
    };

    /**
     * @brief Data location patterns for common apps
     */
    struct DataLocation {
        QString pattern;          // Name pattern (e.g., "Google Chrome")
        QStringList paths;        // Possible data paths
        QString description;      // User-friendly description
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

    /**
     * @brief Extract zip archive
     */
    bool extractArchive(const QString& archive_path,
                       const QString& destination,
                       const RestoreConfig& config);

    /**
     * @brief Check if path matches exclusion patterns
     */
    bool isExcluded(const QString& path, const QStringList& patterns) const;

    /**
     * @brief Get standard app data locations
     */
    QStringList getStandardDataPaths() const;

    /**
     * @brief Recursively copy directory
     */
    bool copyDirectory(const QString& source,
                      const QString& destination,
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
};

} // namespace sak
