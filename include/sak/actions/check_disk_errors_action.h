// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Check Disk Errors Action
 *
 * Runs CHKDSK /scan on all drives to detect file system errors.
 */
class CheckDiskErrorsAction : public QuickAction {
    Q_OBJECT

public:
    explicit CheckDiskErrorsAction(QObject* parent = nullptr);

    QString name() const override { return "Check Disk Errors"; }
    QString description() const override { return "Run CHKDSK scan on all drives"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    /// @brief Result of a CHKDSK scan on a single drive
    struct DriveCheckResult {
        QString letter;
        bool has_errors;
        int errors_found;
        bool needs_reboot_to_fix;
        QString statusMessage;
    };

    QVector<DriveCheckResult> m_drive_results;
    QVector<QString> m_drives;

    void checkDrive(const QString& drive_letter);
    void parseChkdskOutput(const QString& output, DriveCheckResult& result);

    // TigerStyle helpers for execute() decomposition
    bool executeEnumerateVolumes(const QDateTime& start_time,
                                 QVector<QChar>& drives,
                                 QString& report);
    void executeRunChkdsk(const QVector<QChar>& drives,
                          QString& report,
                          int& drives_scanned,
                          int& errors_found,
                          int& errors_fixed);
    void executeBuildReport(const QDateTime& start_time,
                            const QString& report,
                            int drives_scanned,
                            int errors_found,
                            int errors_fixed);
    static QString buildRepairVolumeScript(QChar drive);
    void parseDriveScanResult(const QString& output,
                              QChar drive,
                              QString& report,
                              int& drives_scanned,
                              int& errors_found,
                              int& errors_fixed);
    void appendDriveScanEntry(const QString& drive_letter,
                              const QString& status,
                              bool has_corrupt,
                              bool scan_success,
                              bool reboot_needed,
                              QString& report,
                              int& drives_scanned,
                              int& errors_found);
};

}  // namespace sak
