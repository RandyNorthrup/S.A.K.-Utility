// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Check Disk Errors Action
 *
 * Runs CHKDSK /scan on all drives to detect file system errors.
 */
class CheckDiskErrorsAction : public QuickAction {
    Q_OBJECT

public:
    explicit CheckDiskErrorsAction(QObject* parent = nullptr);

    QString name() const override { return "Check Disk Errors"; }
    QString description() const override { return "Run CHKDSK scan on all drives"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

    // ------------------------------------------------------------------
    // Pure decision seams (public for unit testing; no I/O, no state)
    // ------------------------------------------------------------------

    /// @brief Aggregate outcome across every enumerated drive.
    struct DiskCheckOutcome {
        bool success;       ///< True only if EVERY drive was scanned successfully.
        int drives_failed;  ///< Drives that could not be scanned (timeout/error).
    };

    /// @brief Decide the overall outcome. A drive that timed out or failed to
    /// scan is a failure: success requires all enumerated drives to have been
    /// scanned, and any shortfall is reported as a failed-drive count rather
    /// than hidden behind a single successful drive.
    static DiskCheckOutcome evaluateDiskCheckOutcome(int drives_scanned, int total_drives);

    /// @brief Build the PowerShell that CHECKS a volume for errors. This is a
    /// read-only online scan (Repair-Volume -Scan): it never schedules an
    /// offline fix, so a "Check Disk Errors" run does not mutate the disk. When
    /// corruption is found it reports RepairRecommended, nothing more.
    static QString buildScanVolumeScript(QChar drive);

private:
    /// @brief Parsed state from a single drive scan output block
    struct ParsedDriveState {
        QString drive_letter;
        QString status;
        QString error;               ///< Real exception text from a failed scan, if reported.
        bool has_corrupt = false;
        bool corrupt_known = false;  ///< True once a definitive Detected/NotFound verdict is seen.
        bool scan_success = false;
    };

    /// @brief Aggregated per-run tallies passed to report building.
    struct DiskCheckTotals {
        int drives_scanned = 0;
        int total_drives = 0;
        int errors_found = 0;
        int repairs_recommended = 0;
    };

    QVector<QString> m_drives;

    // execute() helpers -- split to stay under the 70-line function gate
    bool executeEnumerateVolumes(const QDateTime& start_time,
                                 QVector<QChar>& drives,
                                 QString& report);
    void executeRunChkdsk(const QVector<QChar>& drives,
                          QString& report,
                          int& drives_scanned,
                          int& errors_found,
                          int& repairs_recommended);
    void executeBuildReport(const QDateTime& start_time,
                            const QString& report,
                            const DiskCheckTotals& totals);
    void parseDriveScanResult(const QString& output,
                              QString& report,
                              int& drives_scanned,
                              int& errors_found,
                              int& repairs_recommended);
    static void processScanKeyValue(const QString& key,
                                    const QString& value,
                                    ParsedDriveState& state,
                                    int& repairs_recommended);
    void appendDriveScanEntry(const ParsedDriveState& state,
                              QString& report,
                              int& drives_scanned,
                              int& errors_found);
};

}  // namespace sak
