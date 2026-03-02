// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QDateTime>
#include <QString>
#include <QStringList>

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

    /// @brief Execute DNS flush and Winsock backup phase
    /// @return false if cancelled
    bool executeFlushDns(QStringList& errors);

    /// @brief Execute Winsock and TCP/IP reset phase
    /// @return false if cancelled
    bool executeResetWinsock(QStringList& errors);

    /// @brief Execute IP renewal, firewall and adapter reset phase
    /// @return false if cancelled
    bool executeResetIpStack(QStringList& errors);

    /// @brief Execute adapter restart and NetBIOS cache clearing
    /// @return false if cancelled
    bool executeResetAdaptersAndCache(QStringList& errors);

    /// @brief Build and emit the final execution report
    void executeBuildReport(const QStringList& errors, const QDateTime& start_time);
};

} // namespace sak

