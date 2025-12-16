// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
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
    struct DriveCheckResult {
        QString letter;
        bool has_errors;
        int errors_found;
        bool needs_reboot_to_fix;
        QString status_message;
    };

    QVector<DriveCheckResult> m_drive_results;
    QVector<QString> m_drives;

    void checkDrive(const QString& drive_letter);
    void parseChkdskOutput(const QString& output, DriveCheckResult& result);
};

} // namespace sak

