// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Verify System Files Action
 * 
 * Runs SFC (System File Checker) and DISM to repair Windows system files.
 */
class VerifySystemFilesAction : public QuickAction {
    Q_OBJECT

public:
    explicit VerifySystemFilesAction(QObject* parent = nullptr);

    QString name() const override { return "Verify System Files"; }
    QString description() const override { return "Run SFC and DISM to repair system files"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    enum class ScanPhase {
        CheckHealth,
        RestoreHealth,
        SFC
    };

    ScanPhase m_current_phase;
    bool m_corruption_detected{false};
    QString m_log_path;
    bool m_sfc_found_issues{false};
    bool m_sfc_repaired{false};
    bool m_dism_successful{false};
    bool m_dism_repaired_issues{false};
    QString m_cbs_log_path;

    void runDISM();
    void runDismCheckHealth();
    void runDismRestoreHealth();
    void runSFC();
    void parseResults(const QString& output);
};

} // namespace sak

