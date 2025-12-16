// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Export Registry Keys Action
 * 
 * Exports critical registry keys to .reg files for emergency recovery.
 */
class ExportRegistryKeysAction : public QuickAction {
    Q_OBJECT

public:
    explicit ExportRegistryKeysAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Export Registry Keys"; }
    QString description() const override { return "Backup critical registry keys"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    qint64 m_total_size{0};
    int m_keys_exported{0};

    void exportKey(const QString& key_path, const QString& filename);
};

} // namespace sak

