// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/action_constants.h"
#include "sak/network_reset_safeguard.h"
#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace sak {

/// One read of every adapter's settings: PowerShell's start plus the queries. A slow guest takes
/// several seconds; the old 5 s verify probe timed out on the VM while its adapters restarted.
inline constexpr int kResetNetworkCaptureTimeoutMs = 45'000;
/// How long, after the adapters restart, the reset keeps reading them: first before it puts
/// settings back (blind, in full, if Windows never answers), then before it gives the check up as
/// "still settling".
inline constexpr int kResetNetworkSettleBudgetMs = 90'000;
inline constexpr int kResetNetworkSettlePollMs = 3000;

/// @brief Where Reset Network keeps what it records, and how long it waits for the adapters.
struct ResetNetworkOptions {
    /// The folder for the settings recorded before the reset. Empty: networkBackupDirectory().
    QString backup_directory;
    int capture_timeout_ms{kResetNetworkCaptureTimeoutMs};
    int settle_budget_ms{kResetNetworkSettleBudgetMs};
    int settle_poll_ms{kResetNetworkSettlePollMs};
};

/**
 * @brief Reset Network Settings Action
 *
 * Resets network adapters, TCP/IP stack, DNS cache, and Winsock catalog. Before any change it
 * records every adapter's settings to a file; afterwards it puts the ones typed in by hand back
 * and checks that every adapter that was up has a usable address or DHCP.
 */
class ResetNetworkAction : public QuickAction {
    Q_OBJECT

public:
    explicit ResetNetworkAction(QObject* parent = nullptr);
    explicit ResetNetworkAction(ResetNetworkOptions options, QObject* parent = nullptr);

    QString name() const override {
        return QString::fromLatin1(quick_action_elevation::kResetNetworkName);
    }
    QString description() const override {
        return "Reset TCP/IP, DNS, Winsock and Windows Firewall rules, and restart network "
               "adapters";
    }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

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
    ResetNetworkOptions m_options;
    bool m_requires_reboot{false};
    QString m_winsock_backup_path;
    QString m_firewall_backup_path;
    QString m_settings_backup_path;
    NetworkSettingsCapture m_before;
    QStringList m_put_back;
    QStringList m_notes;
    QStringList m_verification;
    QString m_warning;

    /// @brief Record every adapter's settings to a file BEFORE any change. If that cannot be done,
    /// abort with a failed result: without it, settings typed in by hand would be lost silently.
    bool executeRecordSettings(const QDateTime& start_time);

    /// @brief Back up the Winsock catalog BEFORE any destructive reset. If the
    /// backup cannot be captured, abort (emit a failed result) so the reset does
    /// not proceed without a rollback reference. @return false to abort execute.
    bool executeBackupWinsock(QStringList& errors, const QDateTime& start_time);

    /// @brief Execute the DNS flush phase. @return false if cancelled
    bool executeFlushDns(QStringList& errors);

    /// @brief Execute Winsock and TCP/IP reset phase. @return false if cancelled
    bool executeResetWinsock(QStringList& errors);

    /// @brief Run one `netsh int <family> reset` and read its answer (readNetshReset).
    void executeNetshReset(const QString& family, const QString& label, QStringList& errors);

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

    /// @brief Put the settings typed in by hand back (planSettingsReapply).
    void executePutBack(QStringList& errors);

    /// @brief Check every adapter that was up, waiting a bounded time for them to settle.
    void executeVerify(QStringList& errors);

    /// @brief Wait, within the options' budget, for a capture @p settled accepts.
    SettleResult settle(const std::function<bool(const NetworkSettingsCapture&)>& settled);

    /// @brief Build and emit the final execution report
    void executeBuildReport(const QStringList& errors, const QDateTime& start_time);
};

}  // namespace sak
