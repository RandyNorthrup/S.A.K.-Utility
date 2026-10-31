// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/action_constants.h"
#include "sak/quick_action.h"

#include <QDeadlineTimer>
#include <QString>

namespace sak {

struct ProcessResult;

/// The one time bound for a whole Verify System Files run (SFC, then the DISM probes and any
/// repair). Every step's own timeout is cut to what is left of it, and the assistant's registry
/// thunk waits exactly this long, so no layer can stop the run at a different moment.
inline constexpr int kVerifySystemFilesTimeoutMs = 60 * 60 * 1000;

/**
 * @brief Verify System Files Action
 *
 * Runs SFC (System File Checker) and DISM to repair Windows system files.
 */
class VerifySystemFilesAction : public QuickAction {
    Q_OBJECT

public:
    explicit VerifySystemFilesAction(QObject* parent = nullptr);

    QString name() const override {
        return QString::fromLatin1(quick_action_elevation::kVerifySystemFilesName);
    }
    QString description() const override { return "Run SFC and DISM to repair system files"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }
    int executionTimeoutMs() const override { return kVerifySystemFilesTimeoutMs; }

    void scan() override;
    void execute() override;

    /// @brief The timeout one step may use: its own cap, cut to what is left of the run's
    /// budget. Zero (never negative) once the budget is spent, which means "do not start".
    static int boundedStepTimeoutMs(int step_cap_ms, qint64 remaining_ms);

    // ------------------------------------------------------------------
    // Pure decision seams (public for unit testing; no I/O, no state)
    // ------------------------------------------------------------------

    /// @brief Decide whether DISM CheckHealth/ScanHealth output reports actual
    /// component-store corruption. The healthy verdict string ("No component
    /// store corruption detected.") itself contains the substring "corruption",
    /// so a naive contains("corruption") flips every clean machine to
    /// needs-repair; this matches the corruption-PRESENT states instead.
    static bool dismReportsCorruption(const QString& dism_output);

    /// @brief Build an unpredictable per-run temp filename for SFC's redirected
    /// stdout, so concurrent runs cannot overwrite each other and a planted
    /// reparse point at a guessed name cannot redirect the elevated write. The
    /// first field is an OS-CSPRNG entropy token (see runSFC); the counter still
    /// guarantees two runs in the same millisecond never collide.
    static QString makeUniqueSfcOutputName(qint64 entropy, qint64 msecs, unsigned counter);

private:
    bool m_sfc_found_issues{false};
    bool m_sfc_repaired{false};
    bool m_sfc_ran{false};
    bool m_dism_successful{false};
    bool m_dism_repaired_issues{false};
    // Each DISM health probe (CheckHealth, ScanHealth) sets its OWN assessed flag, and only when
    // it produced an authoritative verdict. A "no corruption" conclusion requires BOTH probes to
    // have completed -- a single shared flag would let a completed CheckHealth mask a failed
    // ScanHealth and report full health after only a partial verification.
    bool m_dism_check_assessed{false};
    bool m_dism_scan_assessed{false};
    // Set when a step was skipped or cut short because the run's single time budget ran out.
    bool m_time_limit_reached{false};
    QDeadlineTimer m_run_deadline;
    QString m_cbs_log_path;

    /// The timeout for the next step, or 0 (and m_time_limit_reached set) when none is left.
    [[nodiscard]] int nextStepTimeoutMs(int step_cap_ms);
    /// A step that ran on the last of the budget and timed out ended because of the budget.
    void noteStepOutcome(const ProcessResult& proc, int granted_ms, int step_cap_ms);
    void runDISM();
    [[nodiscard]] bool runDismCheckHealth();
    [[nodiscard]] bool runDismScanHealth();
    void runDismRestoreHealth();
    void runSFC();
    void parseResults(const QString& output);
    [[nodiscard]] QString buildSfcSummary() const;
    [[nodiscard]] bool verificationSucceeded() const;
    [[nodiscard]] ExecutionResult buildVerificationResult(const QDateTime& start_time) const;
};

}  // namespace sak
