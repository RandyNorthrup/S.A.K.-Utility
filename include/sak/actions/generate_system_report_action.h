// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDir>
#include <QString>
#include <QStringList>

#include <expected>

class GenerateSystemReportActionTests;

namespace sak {

/**
 * @brief Generate System Report Action
 *
 * Generates a comprehensive UTF-8 text report from PowerShell and Qt system collectors.
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
    // Reads inventory and writes only its own report file: no system change to approve.
    bool isReadOnly() const override { return true; }

    void scan() override;
    void execute() override;
    void setOutputLocation(const QString& output_location) { m_output_location = output_location; }

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
    /// @brief Builds the PowerShell script for hardware, BIOS, network, and Windows product
    /// sections (the WINDOWS ACTIVATION heading and its product fields).
    static QString buildHardwareInfoScript();
    /// @brief Builds the PowerShell that completes WINDOWS ACTIVATION with the real license
    /// status and channel (SoftwareLicensingProduct), reporting a failed query in the section.
    static QString buildActivationInfoScript();

    /// @brief Gathers physical disk and SMART info via Get-PhysicalDisk.
    /// @return Report section text.
    QString gatherStorageInfo();

    /// @brief Builds the PowerShell storage collector script.
    /// @return PowerShell script string.
    static QString buildStorageInfoScript();

    /// @brief Gathers active network adapters and IP configuration.
    /// @return Report section text.
    QString gatherNetworkInfo();

    /// @brief Builds the PowerShell network collector script.
    /// @return PowerShell script string.
    static QString buildNetworkInfoScript();

    /// @brief Gathers Qt system info and mounted volume details.
    /// @return Report section text.
    QString gatherQtAndVolumeInfo() const;

    /// @brief Writes @p report to @p filepath as UTF-8.
    /// @return The size of the file on disk in BYTES -- not QString::size(), which counts UTF-16
    /// code units and disagrees with the file for any non-ASCII content -- or the reason the
    /// save failed, carrying QSaveFile's own error text.
    std::expected<qint64, QString> saveReport(const QString& report, const QString& filepath);

    /// @brief Create @p output_dir when it is missing.
    /// @return Empty on success, or the failure in words for the report's failure log.
    QString ensureOutputDirectory(const QDir& output_dir) const;

    /// @brief Save the report file and emit the final ExecutionResult. @p directory_error, when
    /// set, says the report folder could not be created, and joins a failed save's log.
    void saveReportAndFinish(const QString& report,
                             const QString& filepath,
                             const QDateTime& start_time,
                             const QString& directory_error);
};

}  // namespace sak
