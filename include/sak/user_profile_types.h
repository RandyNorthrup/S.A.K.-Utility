#pragma once

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QVector>
#include <QJsonObject>

namespace sak {

/**
 * @brief Types of folders that can be backed up from user profiles
 */
enum class FolderType {
    Documents,
    Desktop,
    Pictures,
    Videos,
    Music,
    Downloads,
    AppData_Roaming,
    AppData_Local,
    Favorites,
    StartMenu,
    Custom
};

/**
 * @brief Permission handling modes during backup/restore
 */
enum class PermissionMode {
    StripAll,              // Remove all ACLs, inherit from parent (SAFEST)
    PreserveOriginal,      // Keep source ACLs (requires admin, risky)
    AssignToDestination,   // Replace owner SID with dest user
    Hybrid                 // Strip dangerous, keep safe
};

/**
 * @brief User mapping modes for restore
 */
enum class MergeMode {
    ReplaceDestination,    // Overwrite destination user's files
    MergeIntoDestination,  // Combine files with conflict resolution
    CreateNewUser          // Create new user profile
};

/**
 * @brief Conflict resolution strategies when merging
 */
enum class ConflictResolution {
    SkipDuplicate,         // Don't copy if file exists
    RenameWithSuffix,      // Add _username suffix
    KeepNewer,             // Compare timestamps
    KeepLarger,            // Compare file sizes
    PromptUser             // Ask for each conflict
};

/**
 * @brief Selection of a folder to backup with filters
 */
struct FolderSelection {
    FolderType type;
    QString display_name;          // "Documents", "Desktop", etc.
    QString relative_path;         // Relative to profile root
    bool selected;                 // Include in backup?
    QStringList include_patterns;  // ["*"] or specific files
    QStringList exclude_patterns;  // Filters to exclude
    qint64 size_bytes;            // Calculated size
    int file_count;               // Number of files
    
    FolderSelection()
        : type(FolderType::Documents)
        , selected(true)
        , size_bytes(0)
        , file_count(0)
    {}
    
    QJsonObject toJson() const;
    static FolderSelection fromJson(const QJsonObject& json);
};

/**
 * @brief User profile data for backup
 */
struct UserProfile {
    QString username;
    QString sid;                   // Security Identifier
    QString profile_path;          // C:\Users\Username
    bool is_current_user;
    bool is_selected;              // Selected for backup (UI state)
    qint64 total_size_estimated;
    QVector<FolderSelection> folder_selections;
    
    UserProfile()
        : is_current_user(false)
        , is_selected(false)
        , total_size_estimated(0)
    {}
    
    QJsonObject toJson() const;
    static UserProfile fromJson(const QJsonObject& json);
};

/**
 * @brief Smart filter rules for excluding dangerous files
 */
struct SmartFilter {
    // Size limits
    bool enable_file_size_limit;    // Enable file size checking
    bool enable_folder_size_limit;  // Enable folder size checking
    qint64 max_single_file_size;    // Skip files larger than this
    qint64 max_folder_size;          // Warn if folder exceeds
    
    // Pattern exclusions (regex-compatible)
    QStringList exclude_patterns;
    QStringList exclude_folders;
    QStringList dangerous_files;    // NTUSER.DAT, etc.
    
    SmartFilter()
        : enable_file_size_limit(false)  // Optional by default
        , enable_folder_size_limit(false) // Optional by default
        , max_single_file_size(2LL * 1024 * 1024 * 1024)  // 2 GB
        , max_folder_size(50LL * 1024 * 1024 * 1024)      // 50 GB
    {
        initializeDefaults();
    }
    
    void initializeDefaults();
    QJsonObject toJson() const;
    static SmartFilter fromJson(const QJsonObject& json);
};

/**
 * @brief User data backed up from a profile
 */
struct BackupUserData {
    QString username;
    QString sid;
    QString profile_path;
    QVector<FolderSelection> backed_up_folders;
    PermissionMode permissions_mode;
    bool encrypted;
    QString compression;           // "zip", "7z", "none"
    QString checksum_sha256;
    
    BackupUserData()
        : permissions_mode(PermissionMode::StripAll)
        , encrypted(false)
        , compression("zip")
    {}
    
    QJsonObject toJson() const;
    static BackupUserData fromJson(const QJsonObject& json);
};

/**
 * @brief Complete backup manifest
 */
struct BackupManifest {
    // Metadata
    QString version;               // Manifest format version
    QDateTime created;
    QString source_machine;
    QString sak_version;
    QString backup_type;           // "user_profiles"
    
    // Content
    QVector<BackupUserData> users;
    SmartFilter filter_rules;
    qint64 total_backup_size_bytes;
    QString manifest_checksum;
    
    BackupManifest()
        : version("1.0")
        , backup_type("user_profiles")
        , total_backup_size_bytes(0)
    {}
    
    QJsonObject toJson() const;
    static BackupManifest fromJson(const QJsonObject& json);
    
    bool saveToFile(const QString& path) const;
    static BackupManifest loadFromFile(const QString& path);
};

/**
 * @brief Mapping from source user to destination user during restore
 */
struct UserMapping {
    QString source_username;
    QString source_sid;
    QString destination_username;
    QString destination_sid;       // Empty if creating new user
    MergeMode mode;
    ConflictResolution conflict_resolution;
    bool selected;                 // Include in restore?
    
    UserMapping()
        : mode(MergeMode::ReplaceDestination)
        , conflict_resolution(ConflictResolution::RenameWithSuffix)
        , selected(true)
    {}
};

/**
 * @brief Result of a backup or restore operation
 */
struct OperationResult {
    bool success;
    QString message;
    int files_processed;
    int files_skipped;
    int files_failed;
    qint64 bytes_processed;
    QStringList errors;
    QStringList warnings;
    QDateTime started;
    QDateTime completed;
    
    OperationResult()
        : success(false)
        , files_processed(0)
        , files_skipped(0)
        , files_failed(0)
        , bytes_processed(0)
    {}
    
    QString getSummary() const;
};

// Helper functions
QString folderTypeToString(FolderType type);
FolderType stringToFolderType(const QString& str);
QString permissionModeToString(PermissionMode mode);
QString mergeModeToString(MergeMode mode);
QString conflictResolutionToString(ConflictResolution mode);

} // namespace sak
