// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Reset Network Settings Action
 * 
 * Resets network adapters, TCP/IP stack, DNS cache, and Winsock catalog.
 */
class ResetNetworkAction : public QuickAction {
    Q_OBJECT

public:
    explicit ResetNetworkAction(QObject* parent = nullptr);

    QString name() const override { return "Reset Network Settings"; }
    QString description() const override { return "Reset TCP/IP, DNS, and Winsock"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    bool m_requires_reboot{false};

    void flushDNS();
    void resetWinsock();
    void resetTCPIP();
    void releaseRenewIP();
    void resetFirewall();
};

} // namespace sak

