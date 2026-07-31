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
class QSettings;
class TestEmailProfileManager;  // grants the unit test access to the pure .reg-confinement seams

class EmailProfileManager : public QObject {
    Q_OBJECT

    friend class ::TestEmailProfileManager;

public:
    explicit EmailProfileManager(QObject* parent = nullptr);

    /// Discover all installed email clients and their profiles
    void discoverProfiles();

    /// @brief Whether the last discoverProfiles() run read every source cleanly.
    /// False when a registry key or profiles.ini could not be read (QSettings
    /// reported AccessError/FormatError) -- so an empty/partial result can be
    /// reported as an honest failure instead of a masked "no profiles" success.
    [[nodiscard]] bool discoveryReliable() const { return m_discovery_reliable; }

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
    /// Mark discovery unreliable if a QSettings read reported an error.
    void noteSettingsStatus(const QSettings& settings);

    std::atomic<bool> m_cancelled{false};
    /// Single-flight guard (B7-16): discoverProfiles/backupProfiles/restoreProfiles
    /// share m_profiles / m_backup_dest_names / m_cancelled, so only one may run at
    /// a time. A concurrent second call is refused instead of racing those members
    /// and resetting the first operation's cancel flag.
    std::atomic<bool> m_operation_active{false};
    /// Set true at the start of each discoverProfiles(); cleared on any read error.
    bool m_discovery_reliable{true};
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
    /// @brief Decode a .reg file's raw bytes to text (UTF-16LE-with-BOM as written by reg.exe
    /// export, else UTF-8/ANSI). Pure; unit-tested.
    [[nodiscard]] static QString decodeRegFile(const QByteArray& bytes);
    /// @brief True iff a single [key] path (brackets stripped, any leading '-' delete marker
    /// removed) targets the Outlook profile subtree we legitimately back up. Blocks HKLM entirely
    /// and every other HKCU area (Run keys, shell hooks, ...). Pure; unit-tested.
    [[nodiscard]] static bool regKeyPathAllowed(const QString& key_path);
    /// @brief True iff EVERY key section in a .reg file's text targets an allowed prefix (and there
    /// is at least one). A malicious/corrupt backup whose .reg writes outside the Outlook subtree
    /// is refused BEFORE reg.exe import. Pure; unit-tested.
    [[nodiscard]] static bool regContentConfinedToEmailHives(const QString& reg_text);
    /// @brief True iff @p candidate resolves to a real location inside @p root. Fully resolves
    /// junctions/symlinks in the deepest EXISTING ancestor (the leaf may not exist yet) via
    /// GetFinalPathNameByHandleW -- a lexical-only check (and even canonicalFilePath, which does
    /// not follow junctions) let a junction under home redirect QFile::copy outside home. Static +
    /// unit-tested with a real junction.
    [[nodiscard]] static bool destinationWithinRoot(const QString& root, const QString& candidate);
    // Each returns false if a restore step hard-failed (rejected/missing/registry/
    // copy failure) so restoreProfiles counts only cleanly-restored profiles (B7-29).
    // An intentional skip (destination already present) is NOT a failure.
    [[nodiscard]] bool restoreSingleProfile(const QJsonObject& prof, const QString& backup_dir);
    [[nodiscard]] bool restoreRegistryFromManifest(const QJsonObject& prof,
                                                   const QString& backup_dir);
    [[nodiscard]] bool restoreOneDataFile(const QJsonObject& file_obj,
                                          const QString& backup_dir,
                                          const QString& home_root);

    // Thunderbird helpers
    void scanThunderbirdDir(const QDir& dir, QVector<sak::EmailDataFile>& files);
    void scanThunderbirdDirRecursive(const QDir& dir,
                                     QVector<sak::EmailDataFile>& files,
                                     int depth,
                                     int max_depth);
};
