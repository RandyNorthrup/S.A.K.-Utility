// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Disable Startup Programs Action
 * 
 * Shows list of high-impact startup programs and allows user to disable them.
 * Reads from Registry and Task Scheduler startup locations.
 */
class DisableStartupProgramsAction : public QuickAction {
    Q_OBJECT

public:
    explicit DisableStartupProgramsAction(QObject* parent = nullptr);

    QString name() const override { return "Disable Startup Programs"; }
    QString description() const override { return "Manage high-impact startup items"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    struct StartupItem {
        QString name;
        QString command;
        QString location; // Registry, Startup Folder, Task Scheduler
        QString impact; // High, Medium, Low
        bool is_enabled;
    };

    QVector<StartupItem> m_startup_items;
    int m_high_impact_count{0};

    void scanRegistryStartup();
    void scanStartupFolder();
    void scanTaskScheduler();
    QString determineImpact(const QString& name);
};

} // namespace sak

