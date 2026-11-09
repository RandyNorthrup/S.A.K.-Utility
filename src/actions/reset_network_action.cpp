// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file reset_network_action.cpp
/// @brief Implements Windows network stack reset and adapter refresh

#include "sak/actions/reset_network_action.h"

#include "sak/action_constants.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/network_adapter_admin.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>

#include <algorithm>
#include <utility>

namespace sak {

namespace {

QString netshResetError(const QString& label, int exit_code, const NetshResetReading& reading) {
    return QStringLiteral("%1 reset failed (exit %2): %3")
        .arg(label)
        .arg(exit_code)
        .arg(reading.detail);
}

QString indented(const QStringList& lines) {
    QStringList out;
    for (const QString& line : lines) {
        out << QStringLiteral("  ") + line;
    }
    return out.join(QLatin1Char('\n'));
}

}  // namespace

ResetNetworkAction::ResetNetworkAction(QObject* parent)
    : ResetNetworkAction(ResetNetworkOptions{}, parent) {}

ResetNetworkAction::ResetNetworkAction(ResetNetworkOptions options, QObject* parent)
    : QuickAction(parent), m_options(std::move(options)) {}

QString ResetNetworkAction::buildAdapterResetScript() {
    // Restart-NetAdapter is non-terminating: a per-adapter failure would leave
    // $LASTEXITCODE 0 and read as success. Force Stop + try/catch so any adapter
    // error terminates the pipeline and the process exits 1 (recorded as a
    // failure by the caller).
    return QStringLiteral(
        "$ErrorActionPreference = 'Stop'\n"
        "try {\n"
        "    Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object {\n"
        "        Restart-NetAdapter -Name $_.Name -Confirm:$false -ErrorAction Stop\n"
        "    }\n"
        "    exit 0\n"
        "} catch {\n"
        "    Write-Error $_.Exception.Message\n"
        "    exit 1\n"
        "}\n");
}

bool ResetNetworkAction::stepFailed(bool timed_out, int exit_code) {
    return timed_out || exit_code != 0;
}

void ResetNetworkAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Network reset cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    const QDateTime start_time = QDateTime::currentDateTime();
    m_requires_reboot = false;
    m_winsock_backup_path.clear();
    m_firewall_backup_path.clear();
    m_settings_backup_path.clear();
    m_before = {};
    m_put_back.clear();
    m_notes.clear();
    m_verification.clear();
    m_warning.clear();
    QStringList errors;

    // Record every adapter's settings, then capture a Winsock backup, before touching anything
    // destructive; abort the whole reset if either cannot be captured (each emits its own failed
    // result).
    if (!executeRecordSettings(start_time) || !executeBackupWinsock(errors, start_time)) {
        return;
    }
    if (!executeFlushDns(errors)) {
        emitCancelledResult(QStringLiteral("Network reset cancelled by user"), start_time);
        return;
    }
    if (!executeResetWinsock(errors)) {
        emitCancelledResult(QStringLiteral("Network reset cancelled by user"), start_time);
        return;
    }
    if (!executeResetIpStack(errors)) {
        emitCancelledResult(QStringLiteral("Network reset cancelled by user"), start_time);
        return;
    }
    // The stack is reset now. A cancel from here only shortens the waits: stopping without these
    // two steps would leave an adapter set up by hand with no address, the outcome they prevent.
    executePutBack(errors);
    executeVerify(errors);
    executeBuildReport(errors, start_time);
}

bool ResetNetworkAction::executeRecordSettings(const QDateTime& start_time) {
    Q_EMIT executionProgress("Recording every adapter's settings...", progress::kStep3);
    QString problem;
    auto capture = captureNetworkSettings(m_options.capture_timeout_ms,
                                          [this]() { return isCancelled(); });
    if (!capture) {
        problem = capture.error();
    } else {
        const QString folder = m_options.backup_directory.isEmpty() ? networkBackupDirectory()
                                                                    : m_options.backup_directory;
        const auto saved = saveNetworkCapture(*capture, folder);
        if (saved) {
            m_before = *capture;
            m_settings_backup_path = *saved;
            return true;
        }
        problem = saved.error();
    }
    // Fail closed: `netsh int ip reset` clears settings typed in by hand, so a reset with no record
    // of them could leave a customer's PC offline with nothing to put back.
    ExecutionResult result;
    result.success = false;
    result.message = "Network reset aborted: the adapters' settings could not be recorded first";
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.log =
        "Aborted before any change, because the reset clears settings typed in by hand and they "
        "could not be recorded:\n" +
        problem;
    finishWithResult(result, ActionStatus::Failed);
    return false;
}

bool ResetNetworkAction::executeBackupWinsock(QStringList& errors, const QDateTime& start_time) {
    // Step 1: Backup current Winsock configuration BEFORE any destructive reset.
    Q_EMIT executionProgress("Backing up Winsock catalog...", progress::kStep5);

    QTemporaryFile backup_file(QDir::temp().filePath(QStringLiteral("sak_winsock_XXXXXX.txt")));
    QString backup_error;
    if (!backup_file.open()) {
        backup_error = "Failed to create temp file for Winsock backup";
    } else {
        const QString backup_path = backup_file.fileName();
        // Run netsh directly and write its captured stdout to the backup file in
        // process. Wrapping this as `cmd.exe /C "netsh ... > <path>"` would let
        // cmd perform %VAR% expansion and metacharacter (& | < >) parsing on the
        // environment-derived TEMP path, an elevated command-injection vector.
        const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("netsh.exe")),
                                              QStringList() << "winsock" << "show" << "catalog",
                                              sak::kTimeoutNetworkReadMs,
                                              [this]() { return isCancelled(); });
        const QByteArray bytes = proc.std_out.toUtf8();
        if (stepFailed(proc.timed_out, proc.exit_code)) {
            backup_error = QString("Winsock backup failed (exit %1)").arg(proc.exit_code);
        } else if (proc.std_out.trimmed().isEmpty()) {
            // An exit-0 run with no catalog text is not a usable backup; refuse it rather
            // than retaining an empty file as the claimed rollback reference.
            backup_error = "Winsock backup is empty (no catalog captured)";
        } else if (backup_file.write(bytes) != bytes.size() || !backup_file.flush()) {
            // Write through the still-open QTemporaryFile handle so there is no by-name
            // reopen for a reparse/hard-link swap to race (the random name was never
            // exposed). On any failure the QTemporaryFile auto-removes.
            backup_error = "Failed to write Winsock backup file";
        } else {
            backup_file.close();
            backup_file.setAutoRemove(false);
            m_winsock_backup_path = backup_path;
        }
    }

    if (!backup_error.isEmpty()) {
        // Fail closed: do not run a destructive network reset with no rollback
        // reference captured.
        errors << backup_error;
        ExecutionResult result;
        result.success = false;
        result.message = "Network reset aborted: Winsock backup failed";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.log =
            "Aborted before any change because the pre-reset Winsock backup could not "
            "be captured:\n" +
            backup_error;
        finishWithResult(result, ActionStatus::Failed);
        return false;
    }

    return true;
}

bool ResetNetworkAction::executeFlushDns(QStringList& errors) {
    if (isCancelled()) {
        return false;
    }

    // Step 2: Flush DNS cache
    Q_EMIT executionProgress("Flushing DNS cache...", progress::kStep15);
    {
        const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("ipconfig.exe")),
                                              QStringList() << "/flushdns",
                                              sak::kTimeoutNetworkReadMs,
                                              [this]() { return isCancelled(); });
        if (stepFailed(proc.timed_out, proc.exit_code)) {
            errors << QString("DNS flush failed (exit %1)").arg(proc.exit_code);
        }
    }

    return !isCancelled();
}

bool ResetNetworkAction::executeResetWinsock(QStringList& errors) {
    // Step 3: Reset Winsock catalog
    Q_EMIT executionProgress("Resetting Winsock catalog...", progress::kStep30);
    {
        const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("netsh.exe")),
                                              QStringList() << "winsock" << "reset",
                                              sak::kTimeoutNetworkReadMs,
                                              [this]() { return isCancelled(); });
        if (proc.timed_out) {
            errors << "Winsock reset timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("Winsock reset failed (exit %1)").arg(proc.exit_code);
        } else {
            // Only a SUCCESSFUL Winsock reset needs a reboot to take effect. A failed
            // reset made no change, so do not claim a reboot is required for it.
            m_requires_reboot = true;
        }
    }

    if (isCancelled()) {
        return false;
    }

    // Step 4: Reset TCP/IP stack
    Q_EMIT executionProgress("Resetting TCP/IP stack...", progress::kStep45);
    executeNetshReset(QStringLiteral("ip"), QStringLiteral("TCP/IP"), errors);
    executeNetshReset(QStringLiteral("ipv6"), QStringLiteral("IPv6"), errors);

    return !isCancelled();
}

void ResetNetworkAction::executeNetshReset(const QString& family,
                                           const QString& label,
                                           QStringList& errors) {
    if (isCancelled()) {
        return;
    }
    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("netsh.exe")),
                                          QStringList() << "int" << family << "reset",
                                          sak::kTimeoutNetworkReadMs,
                                          [this]() { return isCancelled(); });
    // netsh exits 1 on a stock Windows 11 when one protected item is refused, although every
    // other item reset and it asks for a restart (VM smoke, 2026-11-09). Read what it printed.
    const NetshResetReading reading = readNetshReset(proc.std_out, proc.exit_code, proc.timed_out);
    switch (reading.verdict) {
    case NetshResetVerdict::Failed:
        errors << netshResetError(label, proc.exit_code, reading);
        if (!proc.std_out.trimmed().isEmpty()) {
            m_notes << QStringLiteral("%1 reset output:\n%2").arg(label, proc.std_out.trimmed());
        }
        return;
    case NetshResetVerdict::DoneRestartRequired:
        m_requires_reboot = true;
        break;
    case NetshResetVerdict::Done:
        break;
    }
    if (!reading.detail.isEmpty()) {
        m_notes << QStringLiteral("%1 reset: %2").arg(label, reading.detail);
    }
}

bool ResetNetworkAction::executeResetIpStack(QStringList& errors) {
    // Step 5: Release and renew IP addresses
    Q_EMIT executionProgress("Releasing IP addresses...", progress::kStep60);
    {
        const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("ipconfig.exe")),
                                              QStringList() << "/release",
                                              sak::kTimeoutNetworkReadMs,
                                              [this]() { return isCancelled(); });
        if (proc.timed_out) {
            errors << "IP release timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("IP release failed (exit %1)").arg(proc.exit_code);
        }
    }

    QThread::msleep(sak::kTimerServiceDelayMs);

    Q_EMIT executionProgress("Renewing IP addresses...", progress::kStep70);
    {
        const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("ipconfig.exe")),
                                              QStringList() << "/renew",
                                              sak::kTimeoutNetworkReadMs,
                                              [this]() { return isCancelled(); });
        if (proc.timed_out) {
            errors << "IP renew timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("IP renew failed (exit %1)").arg(proc.exit_code);
        }
    }

    if (isCancelled()) {
        return false;
    }

    // Step 6: Export custom firewall rules, then reset (only if export succeeded)
    executeResetFirewall(errors);

    if (isCancelled()) {
        return false;
    }

    return executeResetAdaptersAndCache(errors);
}

void ResetNetworkAction::executeResetFirewall(QStringList& errors) {
    Q_EMIT executionProgress("Backing up firewall rules...", progress::kStep75);
    const QString backup = exportFirewallRules(errors);
    if (backup.isEmpty()) {
        // Fail closed for the firewall specifically: without a rules backup, do
        // NOT wipe potentially custom rules. Recorded as an error so overall
        // success reflects it; the rest of the reset still proceeds.
        errors << "Firewall reset skipped: rules backup failed (custom rules preserved)";
        return;
    }

    Q_EMIT executionProgress("Resetting Windows Firewall...", progress::kStep80);
    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("netsh.exe")),
                                          QStringList() << "advfirewall" << "reset",
                                          sak::kTimeoutNetworkReadMs,
                                          [this]() { return isCancelled(); });
    if (stepFailed(proc.timed_out, proc.exit_code)) {
        errors << QString("Firewall reset failed (exit %1)").arg(proc.exit_code);
    }
}

QString ResetNetworkAction::exportFirewallRules(QStringList& errors) {
    // Host the backup in a freshly-made, uniquely-named subdirectory instead of a
    // shared-%TEMP% placeholder. QTemporaryDir creates the directory atomically with a
    // random name (the underlying mkdir fails if the name already exists), so it is owned
    // by this (elevated) process and cannot be pre-seeded by another user. netsh then
    // writes the .wfw for the first time inside a directory only we can write to, which
    // removes the delete-then-recreate window the old create-placeholder/remove/re-export
    // sequence opened.
    QTemporaryDir backup_dir(QDir::temp().filePath(QStringLiteral("sak_fw_XXXXXX")));
    if (!backup_dir.isValid()) {
        errors << "Failed to create temp directory for firewall backup";
        return {};
    }
    // Verify the freshly-created directory is not a reparse point BEFORE netsh writes into
    // it: a symlink/junction here would redirect the elevated write off-target.
    if (QFileInfo(backup_dir.path()).isSymLink()) {
        errors << "Firewall backup directory is a reparse point; refusing to export";
        return {};
    }

    const QString backup_path = backup_dir.filePath(QStringLiteral("firewall_rules.wfw"));
    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("netsh.exe")),
                                          QStringList() << "advfirewall" << "export" << backup_path,
                                          sak::kTimeoutNetworkReadMs,
                                          [this]() { return isCancelled(); });
    const QFileInfo backup_info(backup_path);
    if (stepFailed(proc.timed_out, proc.exit_code) || QFileInfo(backup_dir.path()).isSymLink() ||
        backup_info.isSymLink() || backup_info.size() <= 0) {
        // Re-check the hosting directory AND the exported file for a reparse point after the
        // write: either being a junction/symlink means netsh's elevated write may have landed
        // on a redirected target, so the "backup" is not trustworthy. Refuse it (QTemporaryDir
        // auto-removes any partial output) and do not proceed to wipe the rules.
        errors << QString("Firewall rules export failed (exit %1)").arg(proc.exit_code);
        return {};
    }

    // Keep the backup and its hosting directory on disk as the rollback reference.
    backup_dir.setAutoRemove(false);
    m_firewall_backup_path = backup_path;
    return backup_path;
}

bool ResetNetworkAction::executeResetAdaptersAndCache(QStringList& errors) {
    // Step 7: Reset network adapter settings
    Q_EMIT executionProgress("Resetting network adapters...", progress::kStep85);
    {
        const ProcessResult adapter_reset = runPowerShell(buildAdapterResetScript(),
                                                          sak::kTimeoutNetworkReadMs,
                                                          /*no_profile=*/true,
                                                          /*bypass_policy=*/true,
                                                          [this]() { return isCancelled(); });
        if (stepFailed(adapter_reset.timed_out, adapter_reset.exit_code)) {
            errors << QString("Adapter restart failed (exit %1)").arg(adapter_reset.exit_code);
        }
        if (!adapter_reset.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Adapter restart warning: " + adapter_reset.std_err.trimmed());
        }
    }

    if (isCancelled()) {
        return false;
    }

    // Step 8: Clear NetBIOS cache
    Q_EMIT executionProgress("Clearing NetBIOS cache...", progress::kStep90);
    {
        const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("nbtstat.exe")),
                                              QStringList() << "-R",
                                              sak::kTimeoutNetworkReadMs,
                                              [this]() { return isCancelled(); });
        if (proc.timed_out) {
            errors << "NetBIOS cache clear timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("NetBIOS cache clear failed (exit %1)").arg(proc.exit_code);
        }
    }
    {
        const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("nbtstat.exe")),
                                              QStringList() << "-RR",
                                              sak::kTimeoutNetworkReadMs,
                                              [this]() { return isCancelled(); });
        if (proc.timed_out) {
            errors << "NetBIOS refresh timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("NetBIOS refresh failed (exit %1)").arg(proc.exit_code);
        }
    }

    return !isCancelled();
}

SettleResult ResetNetworkAction::settle(
    const std::function<bool(const NetworkSettingsCapture&)>& settled) {
    QElapsedTimer clock;
    clock.start();
    const SettleProbe probe{
        .capture = [this]() { return captureNetworkSettings(m_options.capture_timeout_ms); },
        .now_ms = [&clock]() { return clock.elapsed(); },
        .sleep_ms = [](int ms) { QThread::msleep(static_cast<unsigned long>(ms)); },
        .cancelled = [this]() { return isCancelled(); }};
    return waitForSettledNetwork(
        probe, settled, m_options.settle_budget_ms, m_options.settle_poll_ms);
}

void ResetNetworkAction::executePutBack(QStringList& errors) {
    if (std::ranges::none_of(m_before.adapters, [](const AdapterSettings& adapter) {
            return adapter.hasStaticSettings() || adapter.ipv4.dhcp;
        })) {
        return;
    }
    Q_EMIT executionProgress("Putting back settings typed in by hand...", progress::kStep90);
    // Windows may still be restarting the adapters: read them once it answers. If it never does
    // within the budget, everything typed in by hand is put back in full.
    const SettleResult read = settle([](const NetworkSettingsCapture&) { return true; });
    QStringList problems;
    const QList<ReapplyStep> steps = planSettingsReapply(m_before, read.capture, &problems);
    errors << problems;
    for (const ReapplyStep& step : steps) {
        const AdapterAdminOutcome outcome = runAdapterCommand(step.netsh_args, step.description);
        if (outcome.succeeded) {
            m_put_back << QStringLiteral("%1: %2").arg(step.adapter, step.description);
        } else {
            errors << QStringLiteral("Could not put back %1 on %2: %3")
                          .arg(step.description, step.adapter, outcome.message);
        }
    }
}

void ResetNetworkAction::executeVerify(QStringList& errors) {
    // Step 9: Verify network configuration
    Q_EMIT executionProgress("Verifying network configuration...", progress::kStep95);
    const SettleResult result = settle([this](const NetworkSettingsCapture& after) {
        return allAdaptersUsable(checkAdaptersAfterReset(m_before, after));
    });
    if (!result.capture.has_value()) {
        // Still settling: the adapters were restarting for the whole wait. That is a restart
        // pending, not a failure -- and the person is told to check after it.
        m_requires_reboot = true;
        m_warning = QStringLiteral(
                        "Windows was still restarting the adapters, so S.A.K. could not check "
                        "them (%1). Check the connection after the restart.")
                        .arg(result.last_error);
        m_verification << QStringLiteral("not checked: the adapters were still restarting");
        return;
    }
    // The lines shown and the verdict come from the same checks, so they cannot disagree.
    for (const AdapterCheck& check : checkAdaptersAfterReset(m_before, *result.capture)) {
        m_verification << QStringLiteral("%1: %2").arg(check.adapter, check.detail);
        if (!check.usable) {
            errors << QStringLiteral("%1 has %2. Its settings from before the reset are in %3")
                          .arg(check.adapter, check.detail, m_settings_backup_path);
        }
    }
    if (m_verification.isEmpty()) {
        m_verification << QStringLiteral("no adapter that was up needed checking");
    }
}

void ResetNetworkAction::executeBuildReport(const QStringList& errors,
                                            const QDateTime& start_time) {
    ExecutionResult result;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.success = errors.isEmpty();
    result.message =
        errors.isEmpty()
            ? "Network settings reset successfully"
            : QString("Network reset completed with %1")
                  .arg(countOf(errors.size(), QStringLiteral("issue"), QStringLiteral("issues")));
    if (!m_put_back.isEmpty()) {
        result.message += QStringLiteral("; settings typed in by hand were put back");
    }
    if (m_requires_reboot) {
        result.message += " - REBOOT REQUIRED for Winsock/TCP-IP changes";
    }
    result.warning = m_warning;
    result.output_path = m_settings_backup_path;

    result.log = QString(
                     "Settings recorded before the reset: %1\nWinsock backup: %2\nFirewall rules "
                     "backup: %3\n")
                     .arg(m_settings_backup_path,
                          m_winsock_backup_path.isEmpty() ? "unavailable" : m_winsock_backup_path,
                          m_firewall_backup_path.isEmpty() ? "unavailable"
                                                           : m_firewall_backup_path);
    if (!m_put_back.isEmpty()) {
        result.log += "\nPut back:\n" + indented(m_put_back) + "\n";
    }
    result.log += "\nVerification:\n" + indented(m_verification) + "\n";
    if (!m_notes.isEmpty()) {
        result.log += "\nNotes:\n" + indented(m_notes) + "\n";
    }
    if (!errors.isEmpty()) {
        result.log += "\nErrors:\n" + errors.join("\n");
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

}  // namespace sak
