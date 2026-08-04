// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QVector>

class QTextStream;
class BackupBitlockerKeysActionTests;  // unit-test friend (global namespace)

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

    friend class ::BackupBitlockerKeysActionTests;

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
     * @param query_ok Set true only when the query ran AND parsed successfully. A genuinely
     *        empty result (no BitLocker volumes) is still query_ok=true; a denied/failed query
     *        is query_ok=false so callers never treat "unknown" as "no BitLocker".
     * @return Vector of VolumeInfo for each encrypted volume
     */
    QVector<VolumeInfo> detectEncryptedVolumes(bool& query_ok);

    /// @brief Parse PowerShell JSON output into VolumeInfo vector. @p parse_ok is true only on
    ///        well-formed output (a genuinely empty result is a success); malformed or
    ///        wrong-shaped JSON leaves it false so a parse failure is not read as "no volumes".
    QVector<VolumeInfo> parseDetectedVolumes(const QString& output, bool& parse_ok);

    /**
     * @brief Retrieve key protectors for a specific volume
     * @param drive_letter Drive letter (e.g., "C:")
     * @param query_ok Set true only when the query ran successfully (an empty
     *        result is still a success); false on process/parse failure so the
     *        caller can fail closed rather than silently omitting the volume.
     * @return Vector of key protector details
     */
    QVector<KeyProtectorInfo> getKeyProtectors(const QString& drive_letter, bool& query_ok);

    /// @brief Build the PowerShell script to query key protectors for a volume.
    QString buildKeyProtectorScript(const QString& drive_letter) const;
    /// @brief Parse JSON response from key protector query into KeyProtectorInfo vector.
    /// @param parse_ok Set true on valid JSON, false on a parse error.
    QVector<KeyProtectorInfo> parseKeyProtectorResponse(const QString& output, bool& parse_ok);

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
    static void writeRecoveryDocumentFooter(QTextStream& out);

    /**
     * @brief Write individual per-volume key files
     * @param backup_dir Target directory for the backup
     * @param files_written Out: number of key files successfully written
     * @return True if every applicable key file was written and committed;
     *         false (fail closed) if any recovery key file could not be
     *         durably written -- a truncated key file is never left behind.
     */
    bool writePerVolumeKeyFiles(const QString& backup_dir, int& files_written);

    /// @brief Write and atomically commit one volume's recovery key file.
    bool writeOneVolumeKeyFile(const QString& backup_dir, const VolumeInfo& vol);

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
     * @return Formatted timestamp (yyyyMMdd_HHmmss_zzz, millisecond resolution)
     */
    static QString backupTimestamp();

    /// @brief Build a collision-resistant backup directory name. The millisecond
    /// timestamp plus a monotonic counter makes two same-second backups distinct.
    static QString uniqueBackupDirName(const QString& timestamp, unsigned counter);

    /// @brief Outcome of the recovery-key gate that decides whether a backup has
    /// anything worth persisting.
    enum class KeyGate {
        Ok,                  ///< At least one recovery password was captured.
        NoProtectors,        ///< No key protectors could be read (needs admin).
        NoRecoveryPasswords  ///< Protectors exist but none is a recovery password.
    };

    /// @brief Decide the backup gate. A backup is only meaningful when at least
    /// one numerical recovery password was captured; TPM-only protectors cannot
    /// be used as a standalone recovery secret, so they must not gate the
    /// operation to "success" with zero recovery keys saved.
    static KeyGate evaluateKeyGate(int total_keys_found, int total_recovery_passwords);

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
    /// @brief Evaluate the recovery-key gate and emit the matching failure.
    bool extractKeysGatePassed(const QDateTime& start_time,
                               int total_keys_found,
                               int total_recovery_passwords);
    bool executeSaveKeyFiles(const QDateTime& start_time,
                             QString& backup_dir_path,
                             int& key_files_written,
                             bool& permissions_set);
    /// @brief Create a fresh, uniquely named backup directory (fail closed on
    /// any collision so a prior backup is never reopened and truncated).
    bool createBackupDirectory(const QDateTime& start_time, QString& backup_dir_path);
    /// @brief Harden the backup directory ACL; on failure, delete the exposed
    /// key backup and fail closed rather than leaving plaintext keys readable.
    bool secureBackupDirectory(const QDateTime& start_time,
                               const QString& backup_dir_path,
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
    static void writeVolumeKeyEntries(QTextStream& out,
                                      const QVector<KeyProtectorInfo>& protectors);
    /// @brief Build JSON object for a single volume
    QJsonObject buildVolumeJson(const VolumeInfo& vol) const;
};

}  // namespace sak
