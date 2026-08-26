// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>
#include <QStringList>

class GenerateSystemReportActionTests;

namespace sak {

/**
 * @brief Generate System Report Action
 *
 * Generates comprehensive HTML system report using msinfo32 and PowerShell.
 */
class GenerateSystemReportAction : public QuickAction {
    // Test seam, matching this tree's convention (DriveScannerTests, FileScannerTests,
    // PackageMatcherTests, TestMboxWriter, ...). saveReport writes the report as UTF-8 and now
    // reports the BYTE count it wrote; that unit is the whole point of the fix, so it needs a
    // test that can call it. Widening the public API to reach it would not be behaviour-
    // preserving; a friend declaration is.
    friend class ::GenerateSystemReportActionTests;

    Q_OBJECT

public:
    explicit GenerateSystemReportAction(const QString& output_location, QObject* parent = nullptr);

    QString name() const override { return "Generate System Report"; }
    QString description() const override { return "Create comprehensive system report"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Troubleshooting; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

    // ------------------------------------------------------------------
    // Pure decision seams (public for unit testing; no I/O, no state)
    // ------------------------------------------------------------------

    /// @brief A PowerShell collector failed if it timed out, exited non-zero, or
    /// produced no/truncated output (@p output_incomplete). Such a run yields
    /// empty/partial output that must not be silently saved as a complete report.
    static bool collectorFailed(bool timed_out, int exit_code, bool output_incomplete = false);

    /// @brief Report generation only succeeds when the file was written AND
    /// every collector ran; a saved-but-empty report is a failure to surface.
    static bool reportGenerationSucceeded(bool save_ok, bool all_collectors_ok);

private:
    QString m_output_location;
    QString m_report_path;
    /// @brief Names of collectors that failed to run during the current execute().
    QStringList m_collector_errors;

    /// @brief Fail-closed guard for the output location: when it is blank or
    /// relative, emits the invalid-location failure result and returns true so
    /// execute() aborts before writing to a caller-unintended path.
    /// @return True when the location was rejected and its result already emitted.
    bool rejectInvalidOutputLocation();

    /// @brief Mid-run cancellation check: when execute() has been cancelled,
    /// emits the cancelled result stamped with @p start_time and returns true.
    /// @return True when cancellation was handled and execute() should return.
    bool finishIfCancelled(const QDateTime& start_time);

    /// @brief Builds the report header with box-drawing frame and timestamp.
    /// @return Formatted report header string.
    QString buildReportHeader() const;

    /// @brief Gathers OS, hardware, CPU, memory, BIOS,
    /// network, and activation info via Get-ComputerInfo.
    /// @return Report section text; may contain timeout fallback text.
    QString gatherOsAndHardwareInfo();

    /// @brief Builds the PowerShell script for OS and computer system info sections.
    static QString buildOsInfoScript();
    /// @brief Builds the PowerShell script for hardware, BIOS, network, and activation sections.
    static QString buildHardwareInfoScript();

    /// @brief Gathers physical disk and SMART info via Get-PhysicalDisk.
    /// @return Report section text.
    QString gatherStorageInfo();

    /// @brief Gathers active network adapters and IP configuration.
    /// @return Report section text.
    QString gatherNetworkInfo();

    /// @brief Gathers Qt system info and mounted volume details.
    /// @return Report section text.
    QString gatherQtAndVolumeInfo() const;

    /// @brief Saves the assembled report to disk with timing footer.
    /// @return True on successful write.
    /// Writes @p report to @p filepath as UTF-8. @p bytes_written receives the number of BYTES
    /// actually written -- not QString::size(), which counts UTF-16 code units and disagrees with
    /// the file on disk for any non-ASCII content.
    bool saveReport(const QString& report, const QString& filepath, qint64* bytes_written);

    /// @brief Save the report file and emit the final ExecutionResult
    void saveReportAndFinish(const QString& report,
                             const QString& filepath,
                             const QDateTime& start_time);
};

}  // namespace sak
