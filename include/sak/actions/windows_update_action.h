// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
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
};

} // namespace sak

