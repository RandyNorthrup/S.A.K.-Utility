// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QVector>

class QTextStream;

namespace sak {

/**
 * @brief BitLocker Recovery Key Backup Action
 *
 * Enterprise-grade backup of BitLocker recovery keys for all encrypted
 * volumes on the system. Captures recovery passwords, key protector IDs,
 * encryption method, volume status, and key package data.
 *
 * Saves a comprehensive recovery document with all keys, plus individual
 * per-volume key files in a structured backup directory. Requires
 * administrator privileges to access BitLocker WMI providers.
 *
 * Security: Recovery keys are sensitive. The backup is written to the
 * user-specified backup location with restricted file permissions.
 * Keys are never logged to the application log -- only key protector IDs
 * are logged for audit purposes.
 */
class BackupBitlockerKeysAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupBitlockerKeysAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "BitLocker Key Backup"; }
    QString description() const override {
        return "Backup BitLocker recovery keys for all "
               "encrypted volumes";
    }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    /**
     * @brief Information about a BitLocker key protector
     */
    struct KeyProtectorInfo {
        QString protector_id;       // Key protector GUID
        QString protector_type;     // RecoveryPassword, ExternalKey, TPM, etc.
        QString recovery_password;  // 48-digit numerical recovery password
        QString key_file_name;      // External key file name (if applicable)
    };

    /**
     * @brief Information about a BitLocker-protected volume
     */
    struct VolumeInfo {
        QString drive_letter;           // e.g., "C:"
        QString volume_label;           // User-assigned label
        QString device_id;              // WMI device ID
        QString protection_status;      // On, Off, Unknown
        QString encryption_method;      // XTS-AES-128, XTS-AES-256, etc.
        QString encryption_percentage;  // Encryption progress (0-100%)
        QString lock_status;            // Locked, Unlocked
        QString volume_type;            // OperatingSystem, FixedData, Removable
        qint64 volume_size_bytes{0};    // Total volume size
        QVector<KeyProtectorInfo> key_protectors;
    };

    QString m_backup_location;
    QVector<VolumeInfo> m_volumes;

    /**
     * @brief Detect all BitLocker-encrypted volumes using WMI
     * @return Vector of VolumeInfo for each encrypted volume
     */
    QVector<VolumeInfo> detectEncryptedVolumes();

    /// @brief Parse PowerShell JSON output into VolumeInfo vector
    QVector<VolumeInfo> parseDetectedVolumes(const QString& output);

    /**
     * @brief Retrieve key protectors for a specific volume
     * @param drive_letter Drive letter (e.g., "C:")
     * @return Vector of key protector details
     */
    QVector<KeyProtectorInfo> getKeyProtectors(const QString& drive_letter);

    /// @brief Build the PowerShell script to query key protectors for a volume.
    QString buildKeyProtectorScript(const QString& drive_letter) const;
    /// @brief Parse JSON response from key protector query into KeyProtectorInfo vector.
    QVector<KeyProtectorInfo> parseKeyProtectorResponse(const QString& output);

    /**
     * @brief Write recovery keys to the master backup document
     * @param backup_dir Target directory for the backup
     * @return True if write succeeded
     */
    bool writeRecoveryDocument(const QString& backup_dir);

    /// @brief Write the header section of the recovery document
    void writeRecoveryDocumentHeader(QTextStream& out) const;
    /// @brief Write per-volume key protector sections
    void writeRecoveryDocumentVolumes(QTextStream& out) const;
    /// @brief Write recovery instructions footer
    void writeRecoveryDocumentFooter(QTextStream& out) const;

    /**
     * @brief Write individual per-volume key files
     * @param backup_dir Target directory for the backup
     * @return Number of key files written
     */
    int writePerVolumeKeyFiles(const QString& backup_dir);

    /**
     * @brief Set restrictive file permissions on the backup directory
     * @param path Path to restrict
     * @return True if permissions were set successfully
     */
    bool restrictFilePermissions(const QString& path);

    /**
     * @brief Format encryption method enum to human-readable string
     * @param method_code Numeric encryption method code from WMI
     * @return Human-readable encryption method name
     */
    static QString formatEncryptionMethod(int method_code);

    /**
     * @brief Format protector type enum to human-readable string
     * @param type_code Numeric protector type from WMI
     * @return Human-readable protector type name
     */
    static QString formatProtectorType(int type_code);

    /**
     * @brief Format volume type enum to human-readable string
     * @param type_code Numeric volume type from WMI
     * @return Human-readable volume type name
     */
    static QString formatVolumeType(int type_code);

    /**
     * @brief Generate a timestamp string for file naming
     * @return Formatted timestamp (yyyyMMdd_HHmmss)
     */
    static QString backupTimestamp();

    /// @brief Aggregated BitLocker backup result data for report generation
    struct BitlockerReportData {
        int total_keys_found = 0;
        int total_recovery_passwords = 0;
        QString backup_dir_path;
        int key_files_written = 0;
        bool permissions_set = false;
    };

    // TigerStyle helpers for execute() decomposition
    bool executeDiscoverVolumes(const QDateTime& start_time);
    bool executeExtractKeys(const QDateTime& start_time,
                            int& total_keys_found,
                            int& total_recovery_passwords);
    bool executeSaveKeyFiles(const QDateTime& start_time,
                             QString& backup_dir_path,
                             int& key_files_written,
                             bool& permissions_set);
    void executeBuildReport(const QDateTime& start_time, const BitlockerReportData& data);
    bool writeJsonBackup(const QString& backup_dir_path);

    /// @brief Count recovery passwords in a protector list
    static int countRecoveryPasswords(const QVector<KeyProtectorInfo>& protectors);
    /// @brief Check if any protector has a recovery password
    static bool volumeHasRecoveryPassword(const QVector<KeyProtectorInfo>& protectors);
    /// @brief Write a single key protector entry to the recovery document
    void writeKeyProtectorEntry(QTextStream& out, const KeyProtectorInfo& kp, int index) const;
    /// @brief Write recovery password entries for a volume
    void writeVolumeKeyEntries(QTextStream& out, const QVector<KeyProtectorInfo>& protectors) const;
    /// @brief Build JSON object for a single volume
    QJsonObject buildVolumeJson(const VolumeInfo& vol) const;
};

}  // namespace sak
