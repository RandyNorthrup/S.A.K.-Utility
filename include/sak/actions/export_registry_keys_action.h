// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QDir>
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

    /// @brief Build the PowerShell script for comprehensive registry backup
    QString buildRegistryBackupScript(const QString& backup_path, const QString& timestamp) const;
    /// @brief Finalize the registry export result and emit completion signal
    void finalizeRegistryExportResult(const QDateTime& start_time,
                                      const QDir& backup_dir,
                                      int keys_exported,
                                      qint64 total_size,
                                      const QString& manifest_path,
                                      const QString& accumulated_output);
};

}  // namespace sak
