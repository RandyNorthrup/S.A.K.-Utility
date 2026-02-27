// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QString>
#include <memory>

namespace sak {

/**
 * @brief Backup Activation Keys Action
 * 
 * Backs up Windows and Office product keys using existing LicenseScanner.
 */
class BackupActivationKeysAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupActivationKeysAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Backup Activation Keys"; }
    QString description() const override { return "Backup Windows/Office product keys"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    int m_keys_found{0};

    /// @brief Build the report header with box-drawing and timestamp.
    /// @return Report header string.
    QString buildReportHeader() const;

    /// @brief Query Windows license info via slmgr.vbs and append to report.
    /// @param report Report string to append to.
    /// @param[out] partial_key The last 5 chars of the product key.
    /// @param[out] license_status The license status string.
    void queryWindowsLicense(QString& report, QString& partial_key, QString& license_status);

    /// @brief Query OEM product key from BIOS/UEFI and append to report.
    /// @param report Report string to append to.
    void queryOemKey(QString& report);

    /// @brief Query Microsoft Office licenses via ospp.vbs and append to report.
    /// @param report Report string to append to.
    /// @return Number of Office licenses found.
    int queryOfficeLicenses(QString& report);

    /// @brief Append backup notes, security info, and key limitations to report.
    /// @param report Report string to append to.
    /// @param total_licenses Total count of licenses found.
    /// @param filepath Path where the backup file will be saved.
    void appendBackupNotes(QString& report, int total_licenses, const QString& filepath);

    /// @brief Save the report to disk and build structured output.
    /// @param report The full report content.
    /// @param filepath Destination file path.
    /// @param partial_key Windows partial key (for structured output).
    /// @param license_status Windows license status (for structured output).
    /// @param office_count Office licenses found count.
    /// @return true if save succeeded.
    bool saveReportFile(const QString& report, const QString& filepath,
                       const QString& partial_key, const QString& license_status,
                       int office_count);
};

} // namespace sak

