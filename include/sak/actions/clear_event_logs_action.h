// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Clear Event Logs Action
 * 
 * Archives and clears Windows Event Logs (Application, System, Security).
 * Creates backup .evtx files before clearing.
 */
class ClearEventLogsAction : public QuickAction {
    Q_OBJECT

public:
    explicit ClearEventLogsAction(QObject* parent = nullptr);

    QString name() const override { return "Clear Event Logs"; }
    QString description() const override { return "Archive and clear Windows Event Logs"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    qint64 m_total_log_size{0};
    int m_log_count{0};

    qint64 getEventLogSize(const QString& log_name);
    bool backupEventLog(const QString& log_name);
    bool clearEventLog(const QString& log_name);
};

} // namespace sak

