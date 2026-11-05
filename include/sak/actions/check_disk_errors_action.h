// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/action_constants.h"
#include "sak/process_runner.h"
#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class CheckDiskErrorsActionTests;

namespace sak {

/**
 * @brief Check Disk Errors Action
 *
 * Runs Repair-Volume -Scan on writable drive-letter volumes to detect file system errors without
 *
 * scheduling or applying a repair.
 */
/// How long one run of the check may take across all its drives.
inline constexpr int kCheckDiskErrorsTimeoutMs = 2 * 60 * 60 * 1000;

class CheckDiskErrorsAction : public QuickAction {
    Q_OBJECT

public:
    explicit CheckDiskErrorsAction(QObject* parent = nullptr);

    QString name() const override {
        return QString::fromLatin1(quick_action_elevation::kCheckDiskErrorsName);
    }
    int executionTimeoutMs() const override { return kCheckDiskErrorsTimeoutMs; }
    QString description() const override { return "Run read-only online scans on all drives"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void execute() override;

    // ------------------------------------------------------------------
    // Pure decision seams (public for unit testing; no I/O, no state)
    // ------------------------------------------------------------------

    /// @brief One mounted volume as Windows reports it.
    struct MountedVolume {
        QString root;  ///< "C:/", or a folder for a directory-mounted volume.
        bool valid{false};
        bool read_only{false};
        unsigned int drive_type{0};  ///< GetDriveTypeW's answer.
        QString file_system;         ///< "NTFS", "FAT32"...; empty when Windows did not say.
    };

    /// @brief A drive left out because the online scan cannot read its file system.
    struct UnsupportedVolume {
        QChar drive;
        QString file_system;
    };

    /// @brief The drives a run scans, and the network drives it says it left out.
    struct ScanTargets {
        QVector<QChar> drives;
        QVector<QChar> network_drives;
        QVector<UnsupportedVolume> unsupported;
    };

    /// @brief Sort mounted volumes into what this check scans and what it reports skipping. Pure.
    [[nodiscard]] static ScanTargets selectScanTargets(const QVector<MountedVolume>& volumes);

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

    /// @brief What Repair-Volume -Scan's own result means for this check.
    enum class ScanVerdict {
        NoErrors,      ///< The volume is clean.
        FixedOnline,   ///< Errors were found and the file system fixed them without going offline.
        NeedsRepair,   ///< Errors remain and need a repair this check does not run.
        Inconclusive,  ///< No usable answer: failed, must retry, or a value this check does not
                       ///< know.
    };
    /// @brief Classify one Volume.RepairStatus name from the Storage module. An unknown name is
    ///        Inconclusive, never clean. Pure; unit-testable.
    [[nodiscard]] static ScanVerdict classifyScanResult(const QString& result);

    /// @brief Whether a volume's drive type can be scanned at all. Repair-Volume addresses only a
    ///        local volume, so a mapped network drive would otherwise always read as a failure.
    ///        Pure; unit-testable.
    [[nodiscard]] static bool driveTypeIsScannable(unsigned int drive_type);

    /// @brief Whether Repair-Volume -Scan can read this file system: NTFS and ReFS. FAT, FAT32
    ///        and exFAT refuse with "The specified file system is not supported". An unknown
    ///        (empty) name is tried, and a failure is then reported for that drive. Pure.
    [[nodiscard]] static bool fileSystemIsScannable(const QString& file_system);

protected:
    /// @brief The volumes Windows reports mounted right now.
    [[nodiscard]] virtual QVector<MountedVolume> mountedVolumes() const;
    /// @brief Run one drive's scan script, stopped when @p should_cancel says so.
    [[nodiscard]] virtual ProcessResult runScanProcess(const QString& script,
                                                       int timeout_ms,
                                                       const std::function<bool()>& should_cancel);

private:
    friend class ::CheckDiskErrorsActionTests;

    /// @brief Parsed state from a single drive scan output block
    struct ParsedDriveState {
        QString drive_letter;
        QString status;
        QString error;   ///< Real exception text from a failed scan, if reported.
        QString result;  ///< Repair-Volume's own result, one Volume.RepairStatus name.
    };

    /// @brief Aggregated per-run tallies passed to report building.
    struct DiskCheckTotals {
        int drives_scanned = 0;
        int total_drives = 0;
        int errors_found = 0;
        int repairs_recommended = 0;
    };

    QVector<QString> m_drives;
    /// Drives this run counted but could not check, named in its message.
    QStringList m_unchecked_drives;
    /// Drives on a file system the online scan cannot read ("U: is FAT32"). Counted with the
    /// drives that could not be checked, so the summary's count matches the drives it names, but
    /// they do not fail the run: they are named with the read-only chkdsk that checks them.
    QStringList m_unsupported_drives;

    // execute() helpers -- split to stay under the 70-line function gate
    bool executeEnumerateVolumes(const QDateTime& start_time,
                                 QVector<QChar>& drives,
                                 QString& report);
    void executeRunVolumeScan(const QVector<QChar>& drives,
                              QString& report,
                              int& drives_scanned,
                              int& errors_found,
                              int& repairs_recommended);
    void executeBuildReport(const QDateTime& start_time,
                            const QString& report,
                            const DiskCheckTotals& totals);
    static void noteUnsupportedVolume(ScanTargets* targets,
                                      QChar drive,
                                      const QString& file_system);
    void reportDriveWithNoResult(QChar drive, const QString& std_err, QString& report);
    /// @return false when the output held no complete drive entry at all.
    bool parseDriveScanResult(const QString& output,
                              QString& report,
                              int& drives_scanned,
                              int& errors_found,
                              int& repairs_recommended);
    static void processScanKeyValue(const QString& key,
                                    const QString& value,
                                    ParsedDriveState& state);
    void appendDriveScanEntry(const ParsedDriveState& state,
                              QString& report,
                              int& drives_scanned,
                              int& errors_found,
                              int& repairs_recommended);
};

}  // namespace sak
