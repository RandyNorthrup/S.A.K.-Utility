// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QJsonObject>
#include <QString>

namespace sak {

/**
 * @brief Disable Visual Effects Action
 * 
 * Disables Windows visual effects (animations, transparency, shadows)
 * for improved performance on lower-end systems.
 */
class DisableVisualEffectsAction : public QuickAction {
    Q_OBJECT

public:
    explicit DisableVisualEffectsAction(QObject* parent = nullptr);

    QString name() const override { return "Disable Visual Effects"; }
    QString description() const override { return "Reduce animations for better performance"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    bool m_effects_enabled{false};
    int m_effects_count{0};

    bool areVisualEffectsEnabled();
    bool disableVisualEffects();

    /// @brief Builds the PowerShell script to enumerate current visual effects settings.
    /// @return PowerShell script that outputs JSON of current settings.
    QString buildCheckSettingsScript() const;

    /// @brief Builds the PowerShell script to apply Best Performance registry settings.
    /// @return PowerShell script that outputs CHANGES and TOTAL counts.
    QString buildApplySettingsScript() const;

    /// @brief Builds the current settings analysis section of the report.
    /// @return Report section with current settings values.
    QString buildCurrentSettingsReport(const QJsonObject& current_settings) const;

    /// @brief Builds the optimization summary section of the report.
    /// @return Report section with summary, restart notice, and optimizations list.
    QString buildSummaryReport(int settings_total, int settings_changed, const QString& fx_mode) const;
};

} // namespace sak

