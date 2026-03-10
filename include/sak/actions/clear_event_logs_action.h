// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

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

    // TigerStyle helpers for execute() decomposition
    bool executeEnumerateLogs(const QDateTime& start_time, QString& ps_script);
    QString buildLogScriptInit() const;
    QString buildLogScriptLoop() const;
    bool executeClearLogs(const QDateTime& start_time,
                          const QString& ps_script,
                          int& total_logs,
                          int& cleared_logs,
                          int& total_entries,
                          int& backed_up,
                          QString& backup_path,
                          QStringList& details);
    void executeBuildReport(const QDateTime& start_time,
                            int total_logs,
                            int cleared_logs,
                            int total_entries,
                            int backed_up,
                            const QString& backup_path,
                            const QStringList& details);
    void appendSuccessReport(QString& log_output,
                             int total_logs,
                             int cleared_logs,
                             int total_entries,
                             int backed_up,
                             const QString& backup_path,
                             const QStringList& details,
                             qint64 duration_ms);
    void appendFailureReport(QString& log_output, const QStringList& details);
};

}  // namespace sak
