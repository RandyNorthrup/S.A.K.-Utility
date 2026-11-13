// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows_continued_work.h
/// @brief Steps whose work Windows finishes on its own after their PowerShell is killed.

#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>

#include <cstdint>
#include <optional>

namespace sak {

/// Work a script hands to Windows that Windows carries on with when the PowerShell process that
/// started it is killed. A VM run killed each kind mid-way with Stop-Process -Force (2026-11-09):
/// a full format kept writing in the storage service for half an hour after the app and its
/// helper were gone. cipher.exe /w and convert.exe stop at once when killed, and extending a
/// partition, Clear-Disk, a quick format and Optimize-Volume -Analyze end too quickly to matter,
/// so none of those is listed.
enum class WindowsContinuedWork {
    FullFormat,       ///< Format-Volume -Full (storage job "Format Volume").
    Defragment,       ///< Optimize-Volume -Defrag (storage job "Optimize Volume").
    Retrim,           ///< Optimize-Volume -ReTrim (the same job type as a defrag).
    PartitionShrink,  ///< Resize-Partition to a smaller size (defragsvc moves the data).
    VolumeScan,       ///< Repair-Volume -Scan (storage job "Repair Volume").
};

/// One marker line: a script writes a Begin marker just before such a cmdlet and an End marker
/// just after it.
struct WindowsContinuedMarker {
    bool begin{false};
    WindowsContinuedWork work{WindowsContinuedWork::FullFormat};
    /// Bytes Windows works on (the volume or the partition); 0 when the script could not say.
    uint64_t size_bytes{0};
};

/// A step in its Windows-continued phase, as the application saw that phase begin.
struct WindowsContinuedPhase {
    QString step_summary;
    WindowsContinuedMarker marker;
    QDateTime started_at;
};

/// PowerShell, one line, that runs @p statement between the two markers. The markers go straight
/// to stdout through [Console]::Out: written as pipeline output, an End marker waits behind any
/// object the cmdlet returned, which the formatter holds back until the script ends. The End
/// marker is written from a finally block, so a cmdlet that fails still closes its phase, and only
/// a killed process leaves one open. @p size_expression is PowerShell for the bytes Windows works
/// on, such as "$p.Size" or a number.
[[nodiscard]] QString windowsContinuedStatement(WindowsContinuedWork work,
                                                const QString& size_expression,
                                                const QString& statement);

/// Seconds a script waits after an online file system scan before it goes on or reports done.
/// volsnap.sys (Windows 11 26100) can leave a timer queued in a volume's device object after an
/// online scan, and removing the whole disk (an unplug, an eject, a detach) within seconds frees
/// the object under it: Windows bugchecks (operation proof's crash investigation, 2026-11-12; with
/// a 10 s settle after the scan, 24 rounds of scan-then-detach under heavy load did not).
inline constexpr int kVolumeScanSettleSeconds = 15;

/// PowerShell, one statement, that says why it waits and then waits kVolumeScanSettleSeconds:
/// what follows every online scan (Repair-Volume -Scan, and chkdsk without /f).
[[nodiscard]] QString volumeScanSettleStatement();

/// Whether @p line is a Windows-continued marker line, well formed or not: never text for a person.
[[nodiscard]] bool isWindowsContinuedMarkerLine(const QString& line);

/// The marker @p line carries, or nothing when it is not a marker line.
[[nodiscard]] std::optional<WindowsContinuedMarker> parseWindowsContinuedMarker(
    const QString& line);

/// The phase @p output leaves open -- a Begin marker with no End marker after it -- or nothing.
[[nodiscard]] std::optional<WindowsContinuedMarker> openWindowsContinuedPhase(
    const QString& output);

/// What Windows is doing, for use inside a sentence: "a full format".
[[nodiscard]] QString windowsContinuedWorkText(WindowsContinuedWork work);

/// What Windows is doing and on how much: "a full format (931.51 GB)", or without the size when
/// the script could not say.
[[nodiscard]] QString windowsContinuedWorkDescription(const WindowsContinuedMarker& marker);

/// The text shown while @p phase runs: what it is, that it cannot be stopped and that Windows
/// continues it even if S.A.K. Utility is closed, when it started, and that no time estimate is
/// available.
[[nodiscard]] QString windowsContinuedPhaseNotice(const WindowsContinuedPhase& phase);

/// The note the close prompt shows while @p phase runs: the same facts, and what exiting does
/// to the rest of the work.
[[nodiscard]] QString windowsContinuedExitNote(const WindowsContinuedPhase& phase);

/// A step's result when its process ended while @p marker's phase was open: the step may still
/// be running in Windows, and the rest of it did not run.
[[nodiscard]] QString windowsContinuedStepMessage(const QString& step_summary,
                                                  const WindowsContinuedMarker& marker);

}  // namespace sak

Q_DECLARE_METATYPE(sak::WindowsContinuedPhase)
