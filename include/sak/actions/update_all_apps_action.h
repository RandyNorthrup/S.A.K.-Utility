// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/chocolatey_manager.h"
#include "sak/quick_action.h"

#include <QString>
#include <QVector>

#include <memory>

namespace sak {

/**
 * @brief Update All Apps Action
 *
 * Updates all Chocolatey packages using existing ChocolateyManager.
 */
class UpdateAllAppsAction : public QuickAction {
    Q_OBJECT

public:
    explicit UpdateAllAppsAction(QObject* parent = nullptr);

    QString name() const override { return "Update All Apps"; }
    QString description() const override { return "Update all Chocolatey packages"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    std::unique_ptr<ChocolateyManager> m_choco_manager;
    int m_outdated_count{0};
    QVector<QString> m_outdated_packages;
    bool m_choco_installed{false};

    /// @brief Aggregated results from all update phases
    struct UpdateSummary {
        int winget_available{0};
        int winget_updated{0};
        int store_updated{0};
        int choco_updated{0};
        int total_updated{0};
        bool winget_installed{false};
        bool choco_installed{false};
        QString report;
        QString structured_output;
    };

    bool runWingetUpdate(UpdateSummary& summary, const QDateTime& start_time);
    /// @brief Execute the winget upgrade command and record results
    void executeWingetUpgrade(UpdateSummary& summary);
    bool runStoreUpdate(UpdateSummary& summary);
    bool runChocoUpdate(UpdateSummary& summary, const QDateTime& start_time);
    void buildUpdateReport(UpdateSummary& summary, qint64 duration_ms) const;
    ExecutionResult buildExecutionResult(const UpdateSummary& summary, qint64 duration_ms) const;
    void appendUpdateStatusSection(UpdateSummary& summary) const;
    void appendStructuredOutput(UpdateSummary& summary) const;
};

}  // namespace sak
