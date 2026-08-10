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
    QString description() const override {
        return "Reset TCP/IP, DNS, Winsock and Windows Firewall rules, and restart network "
               "adapters";
    }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

    // ------------------------------------------------------------------
    // Pure decision seams (public for unit testing; no I/O, no state)
    // ------------------------------------------------------------------

    /// @brief PowerShell that restarts every "Up" adapter and FAILS CLOSED
    /// (exit 1) if any adapter errors. Restart-NetAdapter is non-terminating by
    /// default, so a naive pipeline would exit 0 and look successful even when a
    /// per-adapter restart failed; this wraps it in Stop + try/catch.
    static QString buildAdapterResetScript();

    /// @brief A netsh/PowerShell step is a failure if it timed out or exited
    /// non-zero. Used to gate the final success verdict on the verify probe.
    static bool stepFailed(bool timed_out, int exit_code);

private:
    bool m_requires_reboot{false};
    QString m_winsock_backup_path;
    QString m_firewall_backup_path;

    /// @brief Back up the Winsock catalog BEFORE any destructive reset. If the
    /// backup cannot be captured, abort (emit a failed result) so the reset does
    /// not proceed without a rollback reference. @return false to abort execute.
    bool executeBackupWinsock(QStringList& errors, const QDateTime& start_time);

    /// @brief Execute the DNS flush phase. @return false if cancelled
    bool executeFlushDns(QStringList& errors);

    /// @brief Execute Winsock and TCP/IP reset phase. @return false if cancelled
    bool executeResetWinsock(QStringList& errors);

    /// @brief Execute IP renewal, firewall and adapter reset phase.
    /// @return false if cancelled
    bool executeResetIpStack(QStringList& errors);

    /// @brief Export firewall rules first, then reset ONLY if the export
    /// succeeded, so a reset never wipes custom rules with no backup.
    void executeResetFirewall(QStringList& errors);

    /// @brief Export current firewall rules to a temp .wfw file.
    /// @return the backup path, or empty on failure.
    QString exportFirewallRules(QStringList& errors);

    /// @brief Execute adapter restart and NetBIOS cache clearing.
    /// @return false if cancelled
    bool executeResetAdaptersAndCache(QStringList& errors);

    /// @brief Build and emit the final execution report
    void executeBuildReport(const QStringList& errors, const QDateTime& start_time);
};

}  // namespace sak
