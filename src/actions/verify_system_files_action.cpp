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
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>

namespace sak {

namespace {
// A DISM health probe only produced an authoritative verdict if its stdout carries an actual
// component-store corruption-state phrase ("corruption" also covers the healthy "No component
// store corruption detected." verdict; "repairable" covers the corrupt verdict). The generic
// "The operation completed successfully" is DELIBERATELY not accepted: DISM prints it for almost
// any command, so treating it as a health verdict would let output that lacks the real corruption
// line pass as healthy. On elevation refusal / error 740 / timeout, stdout carries none of these,
// so the probe is treated as "did not run" and verification fails closed.
//
// A LOCALIZED verdict used to land in that same bucket. That was SAFE -- it degraded to "could
// not assess" rather than a false clean -- but it also meant this check could never assess the
// component store at all on a non-English Windows. Every DISM invocation here now passes
// /English, which forces English output regardless of system language, so these phrases are what
// DISM prints on any locale. The fail-closed behaviour is unchanged if the switch is ever
// unavailable: the phrases simply do not match and the probe reports "did not run".
constexpr int kSfcQuotedLines = 2;

bool dismProbeCompleted(const ProcessResult& proc) {
    return proc.std_out.contains("corruption", Qt::CaseInsensitive) ||
           proc.std_out.contains("repairable", Qt::CaseInsensitive);
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

VerifySystemFilesAction::SfcVerdict VerifySystemFilesAction::parseSfcOutput(const QString& output) {
    SfcVerdict verdict;
    verdict.found_issues = output.contains("found corrupt files", Qt::CaseInsensitive);
    verdict.repaired = verdict.found_issues &&
                       output.contains("successfully repaired", Qt::CaseInsensitive);
    // SFC only ran authoritatively if it either found corruption or gave a clean bill of health;
    // an elevation refusal / aborted service leaves neither phrase, so ran stays false.
    verdict.ran = verdict.found_issues ||
                  output.contains("did not find any integrity violations", Qt::CaseInsensitive);
    if (!verdict.ran) {
        // SFC's words stay English only on English Windows. Elsewhere its verdict is one this
        // check cannot read, so hand it to the person instead of inventing one. Its last two lines:
        // after a repair the verdict comes second to last, ahead of a line about the log file.
        const QStringList lines = output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                               Qt::SkipEmptyParts);
        QStringList tail;
        for (qsizetype i = lines.size() - 1; i >= 0 && tail.size() < kSfcQuotedLines; --i) {
            const QString line = lines.at(i).trimmed();
            if (!line.isEmpty() && !line.startsWith(QLatin1String("CBS_LOG_PATH:"))) {
                tail.prepend(line);
            }
        }
        verdict.detail = tail.join(QLatin1Char(' '));
        // The English refusals are SFC saying it did not run. Anything else it said could be a
        // clean bill of health in another language.
        static const QStringList kEnglishRefusals = {
            QStringLiteral("You must be an administrator"),
            QStringLiteral("Windows Resource Protection could not"),
            QStringLiteral("system repair pending"),
        };
        verdict.unreadable =
            !verdict.detail.isEmpty() &&
            std::ranges::none_of(kEnglishRefusals, [&output](const QString& refusal) {
                return output.contains(refusal, Qt::CaseInsensitive);
            });
    }
    return verdict;
}

QString VerifySystemFilesAction::sfcOutputReadCommand() {
    return QStringLiteral("Get-Content -LiteralPath $sfcOutput -Encoding Unicode | Write-Output");
}

QString VerifySystemFilesAction::makeUniqueSfcOutputName(qint64 entropy,
                                                         qint64 msecs,
                                                         unsigned counter) {
    return QStringLiteral("sak_sfc_output_%1_%2_%3.txt").arg(entropy).arg(msecs).arg(counter);
}

VerifySystemFilesAction::VerifySystemFilesAction(QObject* parent) : QuickAction(parent) {}

int VerifySystemFilesAction::boundedStepTimeoutMs(int step_cap_ms, qint64 remaining_ms) {
    if (step_cap_ms <= 0 || remaining_ms <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<qint64>(step_cap_ms, remaining_ms));
}

int VerifySystemFilesAction::nextStepTimeoutMs(int step_cap_ms) {
    const int granted = boundedStepTimeoutMs(step_cap_ms, m_run_deadline.remainingTime());
    if (granted == 0) {
        m_time_limit_reached = true;
    }
    return granted;
}

void VerifySystemFilesAction::noteStepOutcome(const ProcessResult& proc,
                                              int granted_ms,
                                              int step_cap_ms) {
    if (proc.timed_out && granted_ms < step_cap_ms) {
        m_time_limit_reached = true;
    }
}

ProcessResult VerifySystemFilesAction::runStep(const QString& script, int timeout_ms) {
    return runPowerShell(script, timeout_ms, true, true, [this]() { return isCancelled(); });
}

void VerifySystemFilesAction::runSFC() {
    Q_EMIT executionProgress("Running System File Checker (SFC)...", progress::kStep10);

    // Enterprise approach: Run SFC with real-time progress monitoring and accumulated output.
    // Redirect to an unpredictable per-run temp file (not a fixed name) so concurrent runs
    // cannot clobber each other and a planted reparse point cannot redirect the write.
    static std::atomic<unsigned> s_sfc_counter{0};
    // Feed a cryptographically-random 63-bit token (OS CSPRNG) as the leading field so the
    // redirect target name cannot be predicted -- a predictable pid/time/counter name could be
    // pre-empted by a reparse point planted at the guessed path to hijack the elevated write. The
    // monotonic counter still guarantees two runs in the same millisecond never collide.
    const qint64 entropy = static_cast<qint64>(QRandomGenerator::system()->generate64() &
                                               0x7F'FF'FF'FF'FF'FF'FF'FFULL);
    const QString sfc_out_name =
        makeUniqueSfcOutputName(entropy,
                                QDateTime::currentMSecsSinceEpoch(),
                                s_sfc_counter.fetch_add(1, std::memory_order_relaxed));
    const QString ps_script =
        QStringLiteral(
            "$sfcOutput = Join-Path $env:TEMP '%1'; "
            "$sys = [System.Environment]::GetFolderPath('System'); "
            "$process = Start-Process -FilePath (Join-Path $sys 'sfc.exe') -ArgumentList "
            "'/scannow' -PassThru "
            "-NoNewWindow -Wait -RedirectStandardOutput $sfcOutput; "
            "%2; "
            "$cbsLog = \"$env:SystemRoot\\Logs\\CBS\\CBS.log\"; "
            "if (Test-Path $cbsLog) { Write-Output \"CBS_LOG_PATH:$cbsLog\" }; "
            "Remove-Item -LiteralPath $sfcOutput -Force -ErrorAction Stop")
            .arg(sfc_out_name, sfcOutputReadCommand());

    Q_EMIT executionProgress("SFC scanning...", progress::kStep25);
    const int granted_ms = nextStepTimeoutMs(sak::kTimeoutSystemRepairMs);
    if (granted_ms == 0) {
        return;
    }
    const ProcessResult proc = runStep(ps_script, granted_ms);
    noteStepOutcome(proc, granted_ms, sak::kTimeoutSystemRepairMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("SFC warning: " + proc.std_err.trimmed());
    }
    if (proc.cancelled) {
        return;
    }
    const QString accumulated_output = proc.std_out;

    // Extract CBS.log path
    const QRegularExpression cbs_log_re("CBS_LOG_PATH:(.+)");
    const QRegularExpressionMatch cbs_match = cbs_log_re.match(accumulated_output);
    if (cbs_match.hasMatch()) {
        m_cbs_log_path = cbs_match.captured(1).trimmed();
    }

    const SfcVerdict verdict = parseSfcOutput(accumulated_output);
    m_sfc_found_issues = verdict.found_issues;
    m_sfc_repaired = verdict.repaired;
    m_sfc_ran = verdict.ran;
    m_sfc_unreadable = verdict.unreadable;
    m_sfc_detail = verdict.detail;
}

bool VerifySystemFilesAction::runDismCheckHealth() {
    const QString script =
        "& (Join-Path ([System.Environment]::GetFolderPath('System')) 'DISM.exe') "
        "/Online /Cleanup-Image /CheckHealth /English; "
        "$LASTEXITCODE";

    // Thread cancellation like runSFC/runDismRestoreHealth so cancel() force-terminates the
    // child; otherwise the probe ignores cancel and the controller's thread->wait(10s) times
    // out and deletes a still-running thread.
    const int granted_ms = nextStepTimeoutMs(sak::kTimeoutDismCheckMs);
    if (granted_ms == 0) {
        return false;  // no budget left: not assessed, so the run cannot claim a clean store
    }
    const ProcessResult proc = runStep(script, granted_ms);
    noteStepOutcome(proc, granted_ms, sak::kTimeoutDismCheckMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("DISM CheckHealth warning: " + proc.std_err.trimmed());
    }
    if (proc.cancelled) {
        return false;  // cancelled probe conservatively reports no corruption
    }
    // Require a clean process outcome AND an authoritative verdict phrase; a timed-out or crashed
    // probe (or one that never reached DISM) is NOT an assessment and must not count.
    if (proc.completedSuccessfully() && dismProbeCompleted(proc)) {
        m_dism_check_assessed = true;
    }
    return dismReportsCorruption(proc.std_out);
}

bool VerifySystemFilesAction::runDismScanHealth() {
    Q_EMIT executionProgress("DISM: Scanning component store...", progress::kStep50);
    const QString script =
        "& (Join-Path ([System.Environment]::GetFolderPath('System')) 'DISM.exe') "
        "/Online /Cleanup-Image /ScanHealth /English";

    const int granted_ms = nextStepTimeoutMs(sak::kTimeoutDismScanMs);
    if (granted_ms == 0) {
        return false;  // no budget left: not assessed, so the run cannot claim a clean store
    }
    const ProcessResult proc = runStep(script, granted_ms);
    noteStepOutcome(proc, granted_ms, sak::kTimeoutDismScanMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("DISM ScanHealth warning: " + proc.std_err.trimmed());
    }
    if (proc.cancelled) {
        return false;  // cancelled probe conservatively reports nothing to repair
    }
    // Require a clean process outcome AND an authoritative verdict phrase; a timed-out or crashed
    // probe (or one that never reached DISM) is NOT an assessment and must not count.
    if (proc.completedSuccessfully() && dismProbeCompleted(proc)) {
        m_dism_scan_assessed = true;
    }
    return dismReportsCorruption(proc.std_out);
}

void VerifySystemFilesAction::runDismRestoreHealth() {
    Q_EMIT executionProgress("DISM: Repairing component store...", progress::kStep65);
    const QString script =
        "& (Join-Path ([System.Environment]::GetFolderPath('System')) 'DISM.exe') "
        "/Online /Cleanup-Image /RestoreHealth /LimitAccess /English";

    Q_EMIT executionProgress("DISM restoring...", progress::kStep75);
    const int granted_ms = nextStepTimeoutMs(sak::kTimeoutSystemRepairMs);
    if (granted_ms == 0) {
        return;
    }
    const ProcessResult proc = runStep(script, granted_ms);
    noteStepOutcome(proc, granted_ms, sak::kTimeoutSystemRepairMs);
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

    const bool corruption_detected = runDismCheckHealth();
    if (isCancelled()) {
        return;
    }

    const bool repair_needed = runDismScanHealth();
    if (isCancelled()) {
        return;
    }

    if (corruption_detected || repair_needed) {
        runDismRestoreHealth();
    } else if (m_dism_check_assessed && m_dism_scan_assessed) {
        // Only claim a clean store when BOTH probes reached an authoritative verdict. If either
        // probe failed to complete, fall through leaving m_dism_successful false (fail closed).
        Q_EMIT executionProgress("DISM: No corruption detected", progress::kStep85);
        m_dism_successful = true;
        m_dism_repaired_issues = false;
    }
}

QString VerifySystemFilesAction::buildSfcSummary() const {
    if (!m_sfc_ran) {
        // SFC never produced an authoritative verdict (e.g. elevation refused or
        // the service aborted). Do NOT claim a clean bill of health -- that is
        // distinct from a completed run that found no violations.
        if (m_sfc_unreadable) {
            return QStringLiteral(
                       "SFC finished, but this check can only read its result in English, so it "
                       "cannot say whether SFC passed. This is not a failure. SFC said: %1 ")
                .arg(m_sfc_detail);
        }
        return m_sfc_detail.isEmpty()
                   ? QStringLiteral("SFC did not complete a verification run. ")
                   : QStringLiteral("SFC did not complete a verification run. It said: %1 ")
                         .arg(m_sfc_detail);
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
    const QDateTime start_time = QDateTime::currentDateTime();
    m_sfc_found_issues = false;
    m_sfc_repaired = false;
    m_sfc_ran = false;
    m_sfc_unreadable = false;
    m_sfc_detail.clear();
    m_dism_successful = false;
    m_dism_repaired_issues = false;
    m_dism_check_assessed = false;
    m_dism_scan_assessed = false;
    m_time_limit_reached = false;
    m_run_deadline = QDeadlineTimer(kVerifySystemFilesTimeoutMs);
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
    if (m_dism_repaired_issues) {
        message += QStringLiteral("DISM repaired component store issues.");
    } else if (m_dism_successful) {
        message += QStringLiteral("DISM found no issues.");
    } else {
        // DISM did not reach an authoritative verdict (launch/elevation failure, timeout, an
        // incomplete probe, or a failed repair). Do NOT claim health -- fail closed in the
        // human-facing message, matching result.success == false.
        message += QStringLiteral("DISM did not complete a component store assessment.");
    }
    if (m_time_limit_reached) {
        message += QStringLiteral(" Stopped at the %1-minute limit for one verification run.")
                       .arg(kVerifySystemFilesTimeoutMs /
                            (sak::kMillisecondsPerSecond * sak::kSecondsPerMinute));
    }

    ExecutionResult result;
    result.success = verificationSucceeded();
    // A healthy PC on non-English Windows lands here, so it must not be told it failed. With DISM
    // also unable to answer there is nothing good to report, and that stays a failure.
    result.inconclusive = m_sfc_unreadable && m_dism_successful;
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
