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

    /// Bundled result from log clearing operation
    struct ClearLogsResult {
        int total_logs = 0;
        int cleared_logs = 0;
        int total_entries = 0;
        int backed_up = 0;
        QString backup_path;
        QStringList details;
    };

    bool executeClearLogs(const QDateTime& start_time,
                          const QString& ps_script,
                          ClearLogsResult& result);
    void parseClearLogsOutput(const QStringList& lines, ClearLogsResult& result) const;
    void executeBuildReport(const QDateTime& start_time, const ClearLogsResult& result);
    void appendSuccessReport(QString& log_output,
                             const ClearLogsResult& result,
                             qint64 duration_ms);
    void appendFailureReport(QString& log_output, const QStringList& details);
};

}  // namespace sak
