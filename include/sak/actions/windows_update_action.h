// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>

namespace sak {

/**
 * @brief Windows Update Action
 *
 * Checks for and installs Windows Updates using PowerShell PSWindowsUpdate module.
 */
class WindowsUpdateAction : public QuickAction {
    Q_OBJECT

public:
    explicit WindowsUpdateAction(QObject* parent = nullptr);

    QString name() const override { return "Windows Update"; }
    QString description() const override { return "Check for and install Windows Updates"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    int m_available_updates{0};
    bool m_requires_reboot{false};
    qint64 m_total_download_size{0};
    bool m_ps_module_installed{false};

    bool isPSWindowsUpdateInstalled();
    bool installPSWindowsUpdateModule();
    void checkForUpdates();
    void installUpdates();

    // TigerStyle helpers for execute() decomposition
    void executeInitSession(const QDateTime& start_time, QString& ps_script);
    static QString buildUpdateScanScript();
    static QString buildUpdateInstallScript();
    bool executeSearchUpdates(const QDateTime& start_time,
                              const QString& ps_script,
                              QString& accumulated_output,
                              QString& errors,
                              int& exit_code);
    void executeBuildReport(const QDateTime& start_time,
                            const QString& accumulated_output,
                            const QString& errors,
                            int exit_code);
};

}  // namespace sak
