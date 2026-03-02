// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file verify_system_files_action.cpp
/// @brief Implements Windows system file verification using SFC and DISM

#include "sak/actions/verify_system_files_action.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"
#include <QRegularExpression>

namespace sak {

VerifySystemFilesAction::VerifySystemFilesAction(QObject* parent)
    : QuickAction(parent)
{
}

void VerifySystemFilesAction::runSFC() {
    Q_EMIT executionProgress("Running System File Checker (SFC)...", 10);

    // Enterprise approach: Run SFC with real-time progress monitoring and accumulated output
    QString ps_script =
        "$sfcOutput = Join-Path $env:TEMP 'sak_sfc_output.txt'; "
        "$process = Start-Process -FilePath 'sfc' -ArgumentList '/scannow' -PassThru -NoNewWindow "
        "-Wait -RedirectStandardOutput $sfcOutput; "
        "Get-Content $sfcOutput | Write-Output; "
        "$cbsLog = \"$env:SystemRoot\\Logs\\CBS\\CBS.log\"; "
        "if (Test-Path $cbsLog) { Write-Output \"CBS_LOG_PATH:$cbsLog\" }; "
        "Remove-Item $sfcOutput -ErrorAction SilentlyContinue";

    Q_EMIT executionProgress("SFC scanning...", 25);
    ProcessResult proc = runPowerShell(ps_script, sak::kTimeoutSystemRepairMs, true, true,
        [this]() { return isCancelled(); });
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("SFC warning: " + proc.std_err.trimmed());
    }
    if (proc.cancelled) {
        return;
    }
    QString accumulated_output = proc.std_out;

    // Extract CBS.log path
    QRegularExpression cbsLogRe("CBS_LOG_PATH:(.+)");
    QRegularExpressionMatch cbsMatch = cbsLogRe.match(accumulated_output);
    if (cbsMatch.hasMatch()) {
        m_cbs_log_path = cbsMatch.captured(1).trimmed();
    }

    if (accumulated_output.contains("found corrupt files", Qt::CaseInsensitive)) {
        m_sfc_found_issues = true;
        m_sfc_repaired = accumulated_output.contains("successfully repaired", Qt::CaseInsensitive);
    }
}

void VerifySystemFilesAction::runDISM() {
    // Enterprise DISM sequence: CheckHealth → ScanHealth → RestoreHealth
    // Per Microsoft docs: Use /LimitAccess to prevent Windows Update contact during repair

    Q_EMIT executionProgress("DISM: Checking component store health...", 35);

    // Step 1: CheckHealth - Quick check for corruption
    QString checkHealthScript =
        "DISM.exe /Online /Cleanup-Image /CheckHealth; "
        "$LASTEXITCODE";

    ProcessResult checkProc = runPowerShell(checkHealthScript, sak::kTimeoutDismCheckMs);
    if (!checkProc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("DISM CheckHealth warning: " + checkProc.std_err.trimmed());
    }

    if (isCancelled()) return;

    QString checkOutput = checkProc.std_out;
    bool corruption_detected = checkOutput.contains("corruption", Qt::CaseInsensitive);

    Q_EMIT executionProgress("DISM: Scanning component store...", 50);

    // Step 2: ScanHealth - Thorough scan for corruption
    QString scanHealthScript = "DISM.exe /Online /Cleanup-Image /ScanHealth";

    ProcessResult scanProc = runPowerShell(scanHealthScript, sak::kTimeoutDismScanMs);
    if (!scanProc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("DISM ScanHealth warning: " + scanProc.std_err.trimmed());
    }

    if (isCancelled()) return;

    QString scanOutput = scanProc.std_out;
    bool repair_needed = scanOutput.contains("repairable", Qt::CaseInsensitive) ||
                        scanOutput.contains("corruption", Qt::CaseInsensitive);

    if (corruption_detected || repair_needed) {
        Q_EMIT executionProgress("DISM: Repairing component store...", 65);

        // Step 3: RestoreHealth - Repair detected corruption
        // /LimitAccess prevents Windows Update usage (offline mode)
        QString restoreHealthScript = "DISM.exe /Online /Cleanup-Image /RestoreHealth /LimitAccess";

        Q_EMIT executionProgress("DISM restoring...", 75);
        ProcessResult restoreProc = runPowerShell(restoreHealthScript, sak::kTimeoutSystemRepairMs,
            true, true, [this]() { return isCancelled(); });
        if (!restoreProc.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("DISM RestoreHealth warning: " + restoreProc.std_err.trimmed());
        }
        if (restoreProc.cancelled) {
            return;
        }
        QString restoreOutput = restoreProc.std_out;

        if (restoreOutput.contains("successfully", Qt::CaseInsensitive)) {
            m_dism_successful = true;
            m_dism_repaired_issues = true;
        }
    } else {
        Q_EMIT executionProgress("DISM: No corruption detected", 85);
        m_dism_successful = true;
        m_dism_repaired_issues = false;
    }
}

void VerifySystemFilesAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to verify system files";
    Q_ASSERT(!result.summary.isEmpty());
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void VerifySystemFilesAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("System file verification cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    m_sfc_found_issues = false;
    m_sfc_repaired = false;
    m_dism_successful = false;
    m_dism_repaired_issues = false;

    runSFC();
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("System file verification cancelled"), start_time);
        return;
    }
    runDISM();
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("System file verification cancelled"), start_time);
        return;
    }

    QString message;
    if (m_sfc_found_issues) {
        if (m_sfc_repaired) {
            message = "SFC found and repaired corrupt files. ";
        } else {
            message = "SFC found corrupt files but could not repair them. ";
        }
    } else {
        message = "SFC found no integrity violations. ";
    }

    if (m_dism_repaired_issues) {
        message += "DISM repaired component store issues.";
    } else {
        message += "DISM found no issues.";
    }

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.success = m_dism_successful && (!m_sfc_found_issues || m_sfc_repaired);
    result.message = message;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.log = QString("SFC issues: %1, repaired: %2\nDISM repaired issues: %3\nCBS log: %4")
        .arg(m_sfc_found_issues ? "YES" : "NO")
        .arg(m_sfc_repaired ? "YES" : "NO")
        .arg(m_dism_repaired_issues ? "YES" : "NO")
        .arg(m_cbs_log_path.isEmpty() ? "N/A" : m_cbs_log_path);

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

} // namespace sak
