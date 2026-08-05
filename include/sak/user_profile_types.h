// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/layout_constants.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak {

inline constexpr qint64 kDefaultMaxSingleFileSizeBytes = 2 * sak::kBytesPerGB;
inline constexpr qint64 kDefaultMaxFolderSizeBytes = 50 * sak::kBytesPerGB;

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
// A "Hybrid" mode was removed here. It was offered in both wizards and described four
// different ways - "Strip dangerous, keep safe" on this enum, "Try Preserve, Fallback
// Strip" in the backup picker, "Safe + Assign" in the restore picker, and "strip then
// assign to the destination user" in PermissionManager - while every path that actually
// consumed it just stripped, exactly like StripAll. A permission control that claims to
// preserve anything and does not is worse than no control. Manifests written with it
// still load: permissionModeFromString maps the legacy "Hybrid" string to StripAll,
// which is what those backups actually got.
enum class PermissionMode {
    StripAll,            // Remove all ACLs, inherit from parent (SAFEST)
    PreserveOriginal,    // Keep source ACLs (requires admin, risky)
    AssignToDestination  // Replace owner SID with dest user
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
    SkipDuplicate,     // Don't copy if file exists
    RenameWithSuffix,  // Add _username suffix
    KeepNewer,         // Compare timestamps
    KeepLarger,        // Compare file sizes
    PromptUser         // Ask for each conflict
};

/**
 * @brief Installed app entry for backup/restore
 */
struct InstalledAppInfo {
    QString name;
    QString version;
    QString publisher;
    QString choco_package;
    QString category;
    bool selected{true};

    QJsonObject toJson() const;
    static InstalledAppInfo fromJson(const QJsonObject& json);
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
    qint64 size_bytes;             // Calculated size
    int file_count;                // Number of files

    FolderSelection() : type(FolderType::Documents), selected(true), size_bytes(0), file_count(0) {}

    QJsonObject toJson() const;
    static FolderSelection fromJson(const QJsonObject& json);
};

/**
 * @brief User profile data for backup
 */
struct UserProfile {
    QString username;
    QString sid;           // Security Identifier
    QString profile_path;  // C:\Users\Username
    bool is_current_user;
    bool is_selected;      // Selected for backup (UI state)
    qint64 total_size_estimated;
    QVector<FolderSelection> folder_selections;

    UserProfile() : is_current_user(false), is_selected(false), total_size_estimated(0) {}

    QJsonObject toJson() const;
    static UserProfile fromJson(const QJsonObject& json);
};

/**
 * @brief Smart filter rules for excluding dangerous files
 */
struct SmartFilter {
    // Size limits
    bool enable_file_size_limit;        // Enable file size checking
    bool enable_folder_size_limit;      // Enable folder size checking
    qint64 max_single_file_size_bytes;  // Skip files larger than this
    qint64 max_folder_size_bytes;       // Warn if folder exceeds

    // Pattern exclusions (regex-compatible)
    QStringList exclude_patterns;
    QStringList exclude_folders;
    QStringList dangerous_files;  // NTUSER.DAT, etc.

    SmartFilter()
        : enable_file_size_limit(false)    // Optional by default
        , enable_folder_size_limit(false)  // Optional by default
        , max_single_file_size_bytes(kDefaultMaxSingleFileSizeBytes)
        , max_folder_size_bytes(kDefaultMaxFolderSizeBytes) {
        initializeDefaults();
    }

    void initializeDefaults();
    QJsonObject toJson() const;
    static SmartFilter fromJson(const QJsonObject& json);

    /// The registry-hive and lock files that MUST always be excluded from a
    /// profile backup (copying a live NTUSER.DAT corrupts it / locks the run).
    /// This set is non-negotiable: initializeDefaults() seeds it and fromJson()
    /// re-unions it, so no supplied/edited manifest can drop it.
    [[nodiscard]] static QStringList mandatoryDangerousFiles();
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
    QString compression;  // "zip", "7z", "none"
    QString checksum_sha256;

    BackupUserData()
        : permissions_mode(PermissionMode::StripAll), encrypted(false), compression("zip") {}

    QJsonObject toJson() const;
    static BackupUserData fromJson(const QJsonObject& json);
};

/**
 * @brief WiFi network profile for backup/restore
 */
struct WifiProfileInfo {
    QString profile_name;   // SSID / profile name
    QString security_type;  // WPA2-Personal, Open, etc.
    QString xml_data;       // Full XML profile data from netsh
    bool selected{true};

    QJsonObject toJson() const;
    static WifiProfileInfo fromJson(const QJsonObject& json);
};

/**
 * @brief Ethernet adapter configuration for backup/restore
 */
struct EthernetConfigInfo {
    QString adapter_name;
    QString description;
    bool dhcp_enabled{true};
    QString ip_address;
    QString subnet_mask;
    QString default_gateway;
    QString dns_primary;
    QString dns_secondary;
    bool selected{true};

    QJsonObject toJson() const;
    static EthernetConfigInfo fromJson(const QJsonObject& json);
};

/**
 * @brief Application data source for backup/restore
 */
struct AppDataSourceInfo {
    QString name;           // e.g. "Chrome Profiles"
    QString category;       // e.g. "Browsers"
    QString relative_path;  // Relative to user profile (e.g. AppData/Local/Google/Chrome)
    qint64 size_bytes{0};
    bool exists{false};
    bool selected{true};

    QJsonObject toJson() const;
    static AppDataSourceInfo fromJson(const QJsonObject& json);
};

/**
 * @brief Complete backup manifest
 */
struct BackupManifest {
    // Metadata
    QString version;  // Manifest format version
    QDateTime created;
    QString source_machine;
    QString sak_version;
    QString backup_type;  // "user_profiles"

    // Content
    QVector<BackupUserData> users;
    SmartFilter filter_rules;
    QVector<WifiProfileInfo> wifi_profiles;
    QVector<EthernetConfigInfo> ethernet_configs;
    QVector<AppDataSourceInfo> app_data_sources;
    qint64 total_backup_size_bytes;
    QString manifest_checksum;

    /// How the payload files were written. Restore needs these to know that a file is a
    /// codec container and that a password will be required; the container magic is the
    /// authority per file, and these are what let the UI say so before reading any file.
    bool compressed{false};
    bool encrypted{false};

    BackupManifest() : version("1.0"), backup_type("user_profiles"), total_backup_size_bytes(0) {}

    QJsonObject toJson() const;
    static BackupManifest fromJson(const QJsonObject& json);

    bool saveToFile(const QString& path) const;
    static BackupManifest loadFromFile(const QString& path);

    // ---- Integrity checksums (B7-13) ---------------------------------------
    // The manifest and each user's backed-up payload are protected by SHA-256
    // digests that are actually computed at backup time and verified at restore
    // time (previously the fields were serialized but never populated/checked).

    /// SHA-256 hex over this manifest's JSON with the "manifest_checksum" key
    /// removed, so the digest never covers itself. Deterministic: QJsonObject
    /// serializes keys in sorted order.
    [[nodiscard]] QString computeManifestChecksum() const;

    /// True if manifest_checksum matches the recomputed digest. An empty stored
    /// checksum (a legacy backup made before this field was populated) is treated
    /// as unverifiable-but-acceptable and returns true; a NON-empty checksum that
    /// does not match returns false (corruption/tamper -> caller fails closed).
    [[nodiscard]] bool verifyManifestChecksum() const;

    /// Pure digest helper: SHA-256 hex of the given object after removing the
    /// self-referential "manifest_checksum" key. Exposed for unit testing.
    [[nodiscard]] static QString checksumOfManifestJson(QJsonObject obj);

    /// Deterministic SHA-256 hex over every regular file under dir_path (sorted by
    /// forward-slash relative path; path + size + bytes folded in). Returns an
    /// empty string when dir_path does not exist. Used for per-user payload
    /// checksum_sha256 at backup and its verification at restore.
    [[nodiscard]] static QString hashDirectoryTree(const QString& dir_path);
};

/**
 * @brief Mapping from source user to destination user during restore
 */
struct UserMapping {
    QString source_username;
    QString source_sid;
    QString destination_username;
    QString destination_sid;  // Empty if creating new user
    MergeMode mode;
    ConflictResolution conflict_resolution;
    bool selected;  // Include in restore?

    UserMapping()
        : mode(MergeMode::ReplaceDestination)
        , conflict_resolution(ConflictResolution::RenameWithSuffix)
        , selected(true) {}
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
        , bytes_processed(0) {}

    QString getSummary() const;
};

// Helper functions
QString folderTypeToString(FolderType type);
FolderType stringToFolderType(const QString& str);
QString permissionModeToString(PermissionMode mode);
QString mergeModeToString(MergeMode mode);
QString conflictResolutionToString(ConflictResolution mode);

}  // namespace sak
