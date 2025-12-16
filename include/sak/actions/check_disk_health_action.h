// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Check Disk Health Action
 * 
 * Checks S.M.A.R.T. status of all drives using wmic or PowerShell.
 */
class CheckDiskHealthAction : public QuickAction {
    Q_OBJECT

public:
    explicit CheckDiskHealthAction(QObject* parent = nullptr);

    QString name() const override { return "Check Disk Health"; }
    QString description() const override { return "Check S.M.A.R.T. status of all drives"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    struct DriveHealth {
        QString letter;
        QString model;
        QString status; // OK, Warning, Critical
        int temperature;
        int percent_lifetime_used;
        QVector<QString> warnings;
    };
    using DiskHealthInfo = DriveHealth;

    QVector<DriveHealth> m_drives;
    QVector<DriveHealth> m_disk_info;

    QVector<DriveHealth> querySmartStatus();
    bool isDriveSSD(const QString& drive);
    void checkSmartStatus();
    QString parseSmartStatus(const QString& output);
};

} // namespace sak

