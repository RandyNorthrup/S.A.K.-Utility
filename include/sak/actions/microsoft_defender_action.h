// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>
#include <QStringList>

#include <expected>

namespace sak {

struct ProcessResult;

/// What one Microsoft Defender action asks MpCmdRun.exe to do.
enum class DefenderOperation {
    QuickScan,
    FullScan,
    SignatureUpdate,
};

/// What a finished MpCmdRun.exe run means. A scan's exit code 2 is its own outcome, never a
/// generic failure and never "clean": Microsoft documents it as malware that was not remediated
/// or needs user action, or scanning errors.
enum class DefenderOutcome {
    Clean,
    ThreatsNeedAttention,
    SignaturesUpdated,
    Failed,
    TimedOut,
    Cancelled,
};

/// Time bounds, one per operation. A scan or update that runs past it is stopped (and a scan is
/// also cancelled inside the Defender service), then reported as timed out.
inline constexpr int kDefenderQuickScanTimeoutMs = 2 * 60 * 60 * 1000;
inline constexpr int kDefenderFullScanTimeoutMs = 4 * 60 * 60 * 1000;
inline constexpr int kDefenderSignatureUpdateTimeoutMs = 30 * 60 * 1000;
/// How long the follow-up "MpCmdRun -Scan -Cancel" may take after a scan was stopped.
inline constexpr int kDefenderScanCancelTimeoutMs = 60 * 1000;
/// MpCmdRun's documented scan return code for "malware found and not remediated, user action
/// required, or scanning errors" (learn.microsoft.com, "Use the command line to manage
/// Microsoft Defender Antivirus").
inline constexpr int kMpCmdRunScanAttentionExitCode = 2;

/**
 * @brief Runs a Microsoft Defender quick scan, full scan or security-intelligence update.
 *
 * MpCmdRun.exe is taken from the platform folder Defender itself registers
 * (HKLM\\SOFTWARE\\Microsoft\\Windows Defender\\InstallLocation), never from PATH or a hard-coded
 * version folder, and only when that folder lies under Defender's own platform or program
 * directory. MpCmdRun must run elevated: like the other administrator quick actions this one
 * runs in the elevated helper when the application is not elevated
 * (QuickActionController::startElevated).
 */
class MicrosoftDefenderAction : public QuickAction {
    Q_OBJECT

public:
    explicit MicrosoftDefenderAction(DefenderOperation operation, QObject* parent = nullptr);

    QString name() const override;
    QString description() const override;
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }
    int executionTimeoutMs() const override { return timeoutMs(m_operation); }

    void scan() override;
    void execute() override;

    // ------------------------------------------------------------------
    // Pure decision seams (public for unit testing; no I/O, no state)
    // ------------------------------------------------------------------

    struct Verdict {
        DefenderOutcome outcome{DefenderOutcome::Failed};
        bool success{false};
        QString message;
    };

    [[nodiscard]] static QString operationName(DefenderOperation operation);
    [[nodiscard]] static int timeoutMs(DefenderOperation operation);
    [[nodiscard]] static QStringList mpCmdRunArguments(DefenderOperation operation);

    /// @brief Map one finished MpCmdRun.exe run to the outcome the caller is told.
    [[nodiscard]] static Verdict verdictFor(DefenderOperation operation,
                                            const ProcessResult& result);

    /// @brief MpCmdRun.exe inside @p install_location, accepted only when that folder is a
    /// platform version folder under @p platform_root or is @p program_root itself (the inbox
    /// copy Defender registers after a platform reset). Anything else -- empty, relative, or a
    /// folder elsewhere -- is refused with the reason.
    [[nodiscard]] static std::expected<QString, QString> mpCmdRunPathFromInstallLocation(
        const QString& install_location, const QString& platform_root, const QString& program_root);

private:
    DefenderOperation m_operation;

    [[nodiscard]] ExecutionResult runMpCmdRun(const QString& mpcmdrun, const QDateTime& started);
    void cancelServiceScan(const QString& mpcmdrun, QStringList* log_lines) const;
};

}  // namespace sak
