#pragma once

#include "sak/user_profile_types.h"
#include <QThread>
#include <QString>
#include <QVector>
#include <QMutex>
#include <atomic>

namespace sak {

class SmartFileFilter;
class PermissionManager;

/**
 * @brief Background worker for restoring user profile backups
 * 
 * Handles:
 * - User mapping (source → destination)
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
    
    /**
     * @brief Start restore operation
     * @param backupPath Path to backup directory
     * @param manifest Backup manifest with source data
     * @param mappings User mappings (source → destination)
     * @param conflictMode How to handle file conflicts
     * @param permMode Permission handling strategy
     * @param verify Verify file integrity after restore
     */
    void startRestore(const QString& backupPath,
                     const BackupManifest& manifest,
                     const QVector<UserMapping>& mappings,
                     ConflictResolution conflictMode,
                     PermissionMode permMode,
                     bool verify);
    
    /**
     * @brief Cancel the restore operation
     */
    void cancel();
    
    /**
     * @brief Check if worker is currently running
     */
    bool isRunning() const { return m_running; }
    
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
    bool restoreFolder(const FolderSelection& folder, const QString& sourcePath, const QString& destPath);
    bool copyFileWithConflictResolution(const QString& source, const QString& dest, qint64 size);
    bool applyPermissions(const QString& filePath, const QString& destinationUser);
    
    // Helpers
    bool validateBackup();
    bool createRestoreStructure();
    qint64 calculateTotalSize();
    void updateProgress(qint64 bytesAdded);
    bool verifyFile(const QString& filePath);
    QString resolveConflict(const QString& sourcePath, const QString& destPath);
    bool copyDirectory(const QString& sourceDir, const QString& destDir, const FolderSelection& folderConfig);
    
    // Data
    QString m_backupPath;
    BackupManifest m_manifest;
    QVector<UserMapping> m_mappings;
    ConflictResolution m_conflictMode;
    PermissionMode m_permissionMode;
    bool m_verify;
    
    // Progress tracking
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_running{false};
    QMutex m_mutex;
    
    qint64 m_totalBytesToRestore{0};
    qint64 m_bytesRestored{0};
    int m_totalFilesToRestore{0};
    int m_filesRestored{0};
    int m_filesSkipped{0};
    int m_filesErrored{0};
    
    // Instances
    SmartFileFilter* m_fileFilter;
    PermissionManager* m_permissionManager;
};

} // namespace sak
