// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QJsonObject>
#include <QString>

class QDateTime;

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

    bool areVisualEffectsEnabled();
    bool disableVisualEffects();

    /// @brief Builds the PowerShell script to enumerate current visual effects settings.
    /// @return PowerShell script that outputs JSON of current settings.
    static QString buildCheckSettingsScript();

    /// @brief Builds the PowerShell script to apply Best Performance registry settings.
    /// @return PowerShell script that outputs CHANGES and TOTAL counts.
    static QString buildApplySettingsScript();

    /// @brief Builds the performance-related registry checks (first 6 settings).
    static QString buildApplyScriptPerformanceChecks();
    /// @brief Builds the desktop/icon registry checks (last 6 settings) and output.
    static QString buildApplyScriptDesktopChecks();

    /// @brief Builds the current settings analysis section of the report.
    /// @return Report section with current settings values.
    QString buildCurrentSettingsReport(const QJsonObject& current_settings) const;

    /// @brief Builds the optimization summary section of the report.
    /// @return Report section with summary, restart notice, and optimizations list.
    QString buildSummaryReport(int settings_total,
                               int settings_changed,
                               const QString& fx_mode) const;

    /// @brief Run apply-settings script and parse CHANGES/TOTAL counts.
    void applyVisualEffectsSettings(QString& report, int& settings_changed, int& settings_total);
    /// @brief Aggregated visual effects optimization result data
    struct VisualEffectsReport {
        int settings_total = 0;
        int settings_changed = 0;
        bool notification_success = false;
        QString fx_mode;
    };

    /// @brief Build structured output and finalize the ExecutionResult.
    void buildAndFinishVisualEffectsResult(const QString& report,
                                           const VisualEffectsReport& ve_report,
                                           const QDateTime& start_time);
};

}  // namespace sak
