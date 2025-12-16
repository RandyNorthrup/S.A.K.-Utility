// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Generate System Report Action
 * 
 * Generates comprehensive HTML system report using msinfo32 and PowerShell.
 */
class GenerateSystemReportAction : public QuickAction {
    Q_OBJECT

public:
    explicit GenerateSystemReportAction(const QString& output_location, QObject* parent = nullptr);

    QString name() const override { return "Generate System Report"; }
    QString description() const override { return "Create comprehensive system report"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Troubleshooting; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_output_location;
    QString m_report_path;

    void gatherSystemInfo();
    void gatherInstalledPrograms();
    void gatherDriverInfo();
    void gatherEventLogs();
    void generateHTML();
};

} // namespace sak

