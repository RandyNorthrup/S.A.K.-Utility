#pragma once

#include "sak/user_profile_types.h"
#include "sak/smart_file_filter.h"
#include "sak/permission_manager.h"
#include <QThread>
#include <QMutex>
#include <atomic>

namespace sak {

/**
 * @brief Background worker for backing up user profiles
 * 
 * Copies selected user profile files/folders to backup destination
 * with smart filtering, permission handling, and progress reporting.
 */
class UserProfileBackupWorker : public QThread {
    Q_OBJECT

public:
    explicit UserProfileBackupWorker(QObject* parent = nullptr);
    ~UserProfileBackupWorker() override;
    
    /**
     * @brief Start backup operation
     * @param manifest Backup configuration and metadata
     * @param users List of users to backup with folder selections
     * @param destinationPath Where to save the backup
     * @param smartFilter Filter rules for excluding files
     * @param permissionMode How to handle file permissions
     */
    void startBackup(const BackupManifest& manifest,
                     const QVector<UserProfile>& users,
                     const QString& destinationPath,
                     const SmartFilter& smartFilter,
                     PermissionMode permissionMode);
    
    /**
     * @brief Cancel the backup operation
     */
    void cancel();
    
    /**
     * @brief Check if backup is currently running
     */
    bool isRunning() const { return m_running; }

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
    bool backupUser(const UserProfile& user, const QString& userBackupPath);
    bool backupFolder(const FolderSelection& folder, 
                     const QString& sourcePath,
                     const QString& destPath);
    bool copyFileWithFiltering(const QString& sourcePath, 
                              const QString& destPath,
                              qint64 fileSize);
    bool applyPermissions(const QString& filePath);
    
    // Helper functions
    bool createBackupStructure();
    bool saveManifest();
    qint64 calculateTotalSize();
    void updateProgress(qint64 bytesAdded);
    
    // Directory operations
    bool copyDirectory(const QString& sourceDir, 
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
    PermissionMode m_permissionMode;
    
    // Progress tracking
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_running{false};
    QMutex m_mutex;
    
    qint64 m_totalBytesToCopy{0};
    qint64 m_bytesCopied{0};
    int m_totalFilesToCopy{0};
    int m_filesCopied{0};
    int m_filesSkipped{0};
    int m_filesErrored{0};
    
    // Instances for operations
    SmartFileFilter* m_fileFilter{nullptr};
    PermissionManager* m_permissionManager{nullptr};
};

} // namespace sak
