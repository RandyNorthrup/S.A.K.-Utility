// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Printer Settings Backup Action
 * 
 * Backs up Windows printer configurations from registry.
 */
class BackupPrinterSettingsAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupPrinterSettingsAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Printer Settings Backup"; }
    QString description() const override { return "Backup printer configurations"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    int m_printers_found{0};

    int countInstalledPrinters();
    bool exportPrinterRegistry(const QString& dest_file);
};

} // namespace sak
