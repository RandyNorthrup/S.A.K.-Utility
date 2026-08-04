// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file verify_system_files_action.cpp
/// @brief Implements Windows system file verification using SFC and DISM

#include "sak/actions/verify_system_files_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QRegularExpression>

#include <atomic>

namespace sak {

namespace {
// A DISM health probe only produced an authoritative verdict if its stdout carries one of the
// known result phrases. On elevation refusal / error 740 / timeout, stdout has none of these,
// so the probe is treated as "did not run" and verification fails closed.
bool dismProbeCompleted(const ProcessResult& proc) {
    return proc.std_out.contains("corruption", Qt::CaseInsensitive) ||
           proc.std_out.contains("repairable", Qt::CaseInsensitive) ||
           proc.std_out.contains("The operation completed successfully", Qt::CaseInsensitive);
}
}  // namespace

bool VerifySystemFilesAction::dismReportsCorruption(const QString& dism_output) {
    // "No component store corruption detected." is the HEALTHY verdict, yet it
    // contains "corruption" -- so exclude it first, then treat a repairable
    // store or any other corruption-detected phrasing as corruption present.
    if (dism_output.contains("No component store corruption", Qt::CaseInsensitive)) {
        return false;
    }
    return dism_output.contains("repairable", Qt::CaseInsensitive) ||
           dism_output.contains("corruption", Qt::CaseInsensitive);
}

QString VerifySystemFilesAction::makeUniqueSfcOutputName(qint64 pid,
                                                         qint64 msecs,
                                                         unsigned counter) {
    return QStringLiteral("sak_sfc_output_%1_%2_%3.txt").arg(pid).arg(msecs).arg(counter);
}

VerifySystemFilesAction::VerifySystemFilesAction(QObject* parent) : QuickAction(parent) {}

void VerifySystemFilesAction::runSFC() {
    Q_EMIT executionProgress("Running System File Checker (SFC)...", progress::kStep10);

    // Enterprise approach: Run SFC with real-time progress monitoring and accumulated output.
    // Redirect to an unpredictable per-run temp file (not a fixed name) so concurrent runs
    // cannot clobber each other and a planted reparse point cannot redirect the write.
    static std::atomic<unsigned> s_sfc_counter{0};
    const QString sfc_out_name =
        makeUniqueSfcOutputName(QCoreApplication::applicationPid(),
                                QDateTime::currentMSecsSinceEpoch(),
                                s_sfc_counter.fetch_add(1, std::memory_order_relaxed));
    QString ps_script =
        QStringLiteral(
            "$sfcOutput = Join-Path $env:TEMP '%1'; "
            "$process = Start-Process -FilePath 'sfc' -ArgumentList '/scannow' -PassThru "
            "-NoNewWindow -Wait -RedirectStandardOutput $sfcOutput; "
            "Get-Content $sfcOutput | Write-Output; "
            "$cbsLog = \"$env:SystemRoot\\Logs\\CBS\\CBS.log\"; "
            "if (Test-Path $cbsLog) { Write-Output \"CBS_LOG_PATH:$cbsLog\" }; "
            "Remove-Item $sfcOutput -ErrorAction SilentlyContinue")
            .arg(sfc_out_name);

    Q_EMIT executionProgress("SFC scanning...", progress::kStep25);
    ProcessResult proc = runPowerShell(
        ps_script, sak::kTimeoutSystemRepairMs, true, true, [this]() { return isCancelled(); });
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
    // SFC only ran authoritatively if it either found corruption or gave a clean bill of health;
    // an elevation refusal / aborted service leaves neither phrase, so m_sfc_ran stays false.
    m_sfc_ran = m_sfc_found_issues ||
                accumulated_output.contains("did not find any integrity violations",
                                            Qt::CaseInsensitive);
}

bool VerifySystemFilesAction::runDismCheckHealth() {
    QString script =
        "DISM.exe /Online /Cleanup-Image /CheckHealth; "
        "$LASTEXITCODE";

    // Thread cancellation like runSFC/runDismRestoreHealth so cancel() force-terminates the
    // child; otherwise the probe ignores cancel and the controller's thread->wait(10s) times
    // out and deletes a still-running thread.
    ProcessResult proc = runPowerShell(script, sak::kTimeoutDismCheckMs, true, true, [this]() {
        return isCancelled();
    });
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("DISM CheckHealth warning: " + proc.std_err.trimmed());
    }
    if (proc.cancelled) {
        return false;  // cancelled probe conservatively reports no corruption
    }
    if (dismProbeCompleted(proc)) {
        m_dism_assessed = true;
    }
    return dismReportsCorruption(proc.std_out);
}

bool VerifySystemFilesAction::runDismScanHealth() {
    Q_EMIT executionProgress("DISM: Scanning component store...", progress::kStep50);
    QString script = "DISM.exe /Online /Cleanup-Image /ScanHealth";

    ProcessResult proc = runPowerShell(script, sak::kTimeoutDismScanMs, true, true, [this]() {
        return isCancelled();
    });
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("DISM ScanHealth warning: " + proc.std_err.trimmed());
    }
    if (proc.cancelled) {
        return false;  // cancelled probe conservatively reports nothing to repair
    }
    if (dismProbeCompleted(proc)) {
        m_dism_assessed = true;
    }
    return dismReportsCorruption(proc.std_out);
}

void VerifySystemFilesAction::runDismRestoreHealth() {
    Q_EMIT executionProgress("DISM: Repairing component store...", progress::kStep65);
    QString script = "DISM.exe /Online /Cleanup-Image /RestoreHealth /LimitAccess";

    Q_EMIT executionProgress("DISM restoring...", progress::kStep75);
    ProcessResult proc = runPowerShell(script, sak::kTimeoutSystemRepairMs, true, true, [this]() {
        return isCancelled();
    });
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("DISM RestoreHealth warning: " + proc.std_err.trimmed());
    }
    if (proc.cancelled) {
        return;
    }

    // Exit code + timeout are authoritative: a bare substring "successfully" also
    // matches DISM's failure line "did not complete successfully". Require a clean
    // exit AND the affirmative phrase "completed successfully" (note the 'd', which
    // the failure phrase "complete successfully" lacks).
    if (proc.completedSuccessfully() &&
        proc.std_out.contains(QStringLiteral("completed successfully"), Qt::CaseInsensitive)) {
        m_dism_successful = true;
        m_dism_repaired_issues = true;
    }
}

void VerifySystemFilesAction::runDISM() {
    Q_EMIT executionProgress("DISM: Checking component store health...", progress::kStep35);

    bool corruption_detected = runDismCheckHealth();
    if (isCancelled()) {
        return;
    }

    bool repair_needed = runDismScanHealth();
    if (isCancelled()) {
        return;
    }

    if (corruption_detected || repair_needed) {
        runDismRestoreHealth();
    } else if (m_dism_assessed) {
        Q_EMIT executionProgress("DISM: No corruption detected", progress::kStep85);
        m_dism_successful = true;
        m_dism_repaired_issues = false;
    }
}

void VerifySystemFilesAction::scan() {
    setStatus(ActionStatus::Scanning);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to verify system files";
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

QString VerifySystemFilesAction::buildSfcSummary() const {
    if (!m_sfc_ran) {
        // SFC never produced an authoritative verdict (e.g. elevation refused or
        // the service aborted). Do NOT claim a clean bill of health -- that is
        // distinct from a completed run that found no violations.
        return QStringLiteral("SFC did not complete a verification run. ");
    }
    if (!m_sfc_found_issues) {
        return QStringLiteral("SFC found no integrity violations. ");
    }
    if (m_sfc_repaired) {
        return QStringLiteral("SFC found and repaired corrupt files. ");
    }
    return QStringLiteral("SFC found corrupt files but could not repair them. ");
}

void VerifySystemFilesAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("System file verification cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    m_sfc_found_issues = false;
    m_sfc_repaired = false;
    m_sfc_ran = false;
    m_dism_successful = false;
    m_dism_repaired_issues = false;
    m_dism_assessed = false;
    m_cbs_log_path.clear();  // never carry a prior run's CBS log path into this run

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

    finishWithResult(buildVerificationResult(start_time),
                     verificationSucceeded() ? ActionStatus::Success : ActionStatus::Failed);
}

bool VerifySystemFilesAction::verificationSucceeded() const {
    return m_sfc_ran && m_dism_successful && (!m_sfc_found_issues || m_sfc_repaired);
}

VerifySystemFilesAction::ExecutionResult VerifySystemFilesAction::buildVerificationResult(
    const QDateTime& start_time) const {
    QString message = buildSfcSummary();
    message += m_dism_repaired_issues ? "DISM repaired component store issues."
                                      : "DISM found no issues.";

    ExecutionResult result;
    result.success = verificationSucceeded();
    result.message = message;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.log = QString(
                     "SFC issues: %1, repaired: %2\n"
                     "DISM repaired issues: %3\nCBS log: %4")
                     .arg(m_sfc_found_issues ? "YES" : "NO",
                          m_sfc_repaired ? "YES" : "NO",
                          m_dism_repaired_issues ? "YES" : "NO",
                          m_cbs_log_path.isEmpty() ? "N/A" : m_cbs_log_path);
    return result;
}

}  // namespace sak
