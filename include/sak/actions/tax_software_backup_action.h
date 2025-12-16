// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Tax Software Data Backup Action
 * 
 * Backs up TurboTax, H&R Block, and other tax software data files.
 */
class TaxSoftwareBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit TaxSoftwareBackupAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Tax Software Data Backup"; }
    QString description() const override { return "Backup TurboTax, H&R Block data"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    struct TaxDataLocation {
        QString software_name;
        QString path;
        qint64 size;
        int file_count;
        int tax_year;
    };

    QString m_backup_location;
    QVector<UserProfile> m_user_profiles;
    QVector<TaxDataLocation> m_tax_data;
    qint64 m_total_size{0};

    void scanTurboTax();
    void scanHRBlock();
    void scanTaxAct();
};

} // namespace sak

