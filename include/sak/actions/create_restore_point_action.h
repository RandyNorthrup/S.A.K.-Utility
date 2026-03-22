// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>

#include <utility>

namespace sak {

/**
 * @brief Create Restore Point Action
 *
 * Creates Windows System Restore point using WMI.
 */
class CreateRestorePointAction : public QuickAction {
    Q_OBJECT

public:
    explicit CreateRestorePointAction(QObject* parent = nullptr);

    QString name() const override { return "Create Restore Point"; }
    QString description() const override { return "Create Windows System Restore point"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    bool m_restore_enabled{false};
    QString m_last_restore_point;

    void checkRestoreStatus();
    void createRestorePoint();

    /// @brief Queries the Volume Shadow Copy service status.
    /// @return Service status string (e.g. "Running", "Stopped").
    QString queryVssServiceStatus();

    /// @brief Queries current restore point count.
    /// @return Count as string, defaults to "0" on failure.
    QString queryRestorePointCount();

    /// @brief Creates a restore point and builds the report section.
    /// @return Pair of (success flag, report section string).
    std::pair<bool, QString> createAndFormatResult(QString& error_msg, QString& error_code);

    /// @brief Verifies the latest restore point and formats details.
    /// @return Report section string with verification results.
    QString verifyLatestRestorePoint();

    /// @brief Builds the PowerShell script to create a restore point.
    /// @return Complete PowerShell script string with error handling.
    static QString buildCreateScript();

    /// @brief Builds the troubleshooting guidance section based on the error code.
    /// @return Report section with issue-specific troubleshooting guidance.
    QString buildTroubleshootingReport(const QString& error_code) const;

    /// @brief Builds the restore point management reference section.
    /// @return Report section with management commands and technical details.
    QString buildManagementReport(const QString& final_count) const;
};

}  // namespace sak
