// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file volume_check_outcome.h
/// @brief What a read-only check of a volume or file system found, in plain words.
///
/// Quick Actions' Check Disk Errors and Partition Manager say a check's finding in the same words:
/// no errors found; errors found, which a read-only check fixed none of; or not checked, and why.
/// Partition Manager's finished dialog said only "Partition operation batch complete" whatever a
/// check had found (the final capture of d8c27a15, fix batch 4).

#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

namespace sak {

struct PartitionFileSystemToolRunResult;

namespace volume_check {

/// What one check found.
enum class Finding {
    NoErrors,     ///< it read the volume and found nothing wrong
    FixedOnline,  ///< Windows found errors and fixed them while the volume stayed online
    ErrorsFound,  ///< it found errors, and being read-only fixed none of them
    NotChecked,   ///< it gave no answer about the volume
};

/// One volume's or file system's outcome: a plain sentence, and whether it calls for a closer look
/// (errors left, or no answer), which a finished dialog shows as a warning.
struct Outcome {
    QString text;
    bool needs_attention{false};
};

/// The route a volume scan took, as its verdict line names it.
inline constexpr QLatin1StringView kOnlineScanRoute{"online scan"};
inline constexpr QLatin1StringView kReadOnlyChkdskRoute{"read-only chkdsk"};

/// Windows' RepairStatus names (the Storage module's Volume.cdxml) as a finding. Anything not
/// listed (Failed, ScanNeedsRetry, the snapshot errors, a name a later Windows adds) is no answer.
[[nodiscard]] Finding repairStatusFinding(const QString& repair_status);

/// "No errors found".
[[nodiscard]] QString noErrorsText();
/// "Errors found and fixed by Windows while the drive stayed online".
[[nodiscard]] QString fixedOnlineText();
/// "Errors found (Windows reports <status>)".
[[nodiscard]] QString windowsReportsErrorsText(const QString& repair_status);
/// "Errors found by a read-only <check>, which fixed none of them".
[[nodiscard]] QString readOnlyErrorsText(const QString& check);
/// "Repair recommended (this check does not modify the disk)".
[[nodiscard]] QString repairRecommendedText();

/// PowerShell that writes a volume scan's verdict line once its route has answered: "Volume
/// check E: online scan: NoErrorsFound". @p variable holds the answer ($sakScanResult, a
/// RepairStatus, or $sakScanVerdict, "None" when chkdsk found nothing).
[[nodiscard]] QString verdictLineStatement(QLatin1StringView route, const QString& variable);

/// The verdict lines in a script's output, one sentence per volume in the order scanned
/// ("Drive E: No errors found (read-only chkdsk).").
[[nodiscard]] QVector<Outcome> volumeOutcomes(const QString& script_output);

/// What a partition recovery scan of disk @p disk_number found: no lost partition, or each
/// candidate boot sector it reported.
[[nodiscard]] Outcome recoveryScanOutcome(uint32_t disk_number, const QString& script_output);

/// What a read-only check of a non-Windows file system (e2fsck -n, fsck_hfs -n) found.
[[nodiscard]] Outcome toolCheckOutcome(const PartitionFileSystemToolRunResult& result,
                                       const QString& file_system);

}  // namespace volume_check
}  // namespace sak
