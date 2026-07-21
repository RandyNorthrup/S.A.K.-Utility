// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_profile_manager.h
/// @brief Discovers, backs up, and restores email client profiles

#pragma once

#include "sak/email_types.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QSet>

#include <atomic>

class QJsonObject;

class EmailProfileManager : public QObject {
    Q_OBJECT

public:
    explicit EmailProfileManager(QObject* parent = nullptr);

    /// Discover all installed email clients and their profiles
    void discoverProfiles();

    /// Backup selected profiles to a target directory
    void backupProfiles(const QVector<int>& profile_indices, const QString& backup_path);

    /// Restore profiles from a backup manifest
    void restoreProfiles(const QString& backup_manifest_path);

    /// Get all linked data file paths (for orphan scanner cross-reference)
    [[nodiscard]] QSet<QString> linkedFilePaths() const;

    void cancel();

Q_SIGNALS:
    void profilesDiscovered(QVector<sak::EmailClientProfile> profiles);
    void backupProgress(int files_done, int total_files, qint64 bytes_copied);
    void backupComplete(QString backup_path, int files_backed_up, qint64 total_bytes);
    void restoreComplete(int profiles_restored);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};
    QVector<sak::EmailClientProfile> m_profiles;

    /// Original data-file path -> the actual on-disk backup basename that was
    /// written for it (only populated on a successful copy). Consumed by
    /// createBackupManifest so the manifest names the real file, not a colliding
    /// original basename or a file that never got copied.
    QHash<QString, QString> m_backup_dest_names;

    // Discovery per client
    QVector<sak::EmailClientProfile> discoverOutlookProfiles();
    QVector<sak::EmailClientProfile> discoverThunderbirdProfiles();
    QVector<sak::EmailClientProfile> discoverWindowsMailProfiles();
    void discoverWmsProfiles(QVector<sak::EmailClientProfile>& results);

    // Backup helpers
    [[nodiscard]] int countTotalDataFiles(const QVector<int>& profile_indices) const;
    [[nodiscard]] bool exportRegistryKey(const QString& key_path, const QString& output_file);
    [[nodiscard]] bool createBackupManifest(const QString& backup_path,
                                            const QVector<sak::EmailClientProfile>& profiles);
    void backupSingleProfile(const sak::EmailClientProfile& profile,
                             const QString& backup_path,
                             int& files_done,
                             int total_files,
                             qint64& bytes_copied);
    [[nodiscard]] static QString uniqueBackupDestination(const QString& backup_path,
                                                         const QFileInfo& source);

    // Restore helpers
    [[nodiscard]] bool importRegistryKey(const QString& reg_file);
    void restoreSingleProfile(const QJsonObject& prof, const QString& backup_dir);
    void restoreRegistryFromManifest(const QJsonObject& prof, const QString& backup_dir);
    void restoreOneDataFile(const QJsonObject& file_obj,
                            const QString& backup_dir,
                            const QString& home_root);

    // Thunderbird helpers
    void scanThunderbirdDir(const QDir& dir, QVector<sak::EmailDataFile>& files);
    void scanThunderbirdDirRecursive(const QDir& dir,
                                     QVector<sak::EmailDataFile>& files,
                                     int depth,
                                     int max_depth);
};
