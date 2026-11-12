// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_stop_cleanup.h
/// @brief What a killed step's script leaves behind, and the cleanup that removes exactly that.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace sak {

/// Something a step's script creates and removes again itself, which a killed script leaves
/// behind: a cancel or a close kills the script's whole process tree, and its finally never runs.
enum class StopLeftoverKind {
    ShadowCopy,    ///< A VSS shadow copy the backup helper made, by its ID ("{GUID}").
    ShadowLink,    ///< The drive letter robocopy reads that copy through, as letter=device.
    CipherFolder,  ///< cipher.exe /w's EFSTMPWP folder, by the letter of the volume it is on.
};

struct StopLeftover {
    StopLeftoverKind kind{StopLeftoverKind::ShadowCopy};
    QString value;  ///< The shadow copy's ID, the link's path, or the volume's drive letter.

    [[nodiscard]] bool operator==(const StopLeftover& other) const = default;
};

/// PowerShell for one statement that writes the marker saying the script has just created the
/// leftover @p kind named by the PowerShell expression @p value_expression.
[[nodiscard]] QString stopLeftoverCreatedStatement(StopLeftoverKind kind,
                                                   const QString& value_expression);

/// PowerShell for one statement that writes the marker saying the script removed it again.
[[nodiscard]] QString stopLeftoverReleasedStatement(StopLeftoverKind kind,
                                                    const QString& value_expression);

/// Follows a step's output a line at a time: what it created and has not removed again.
/// A marker whose value is not exactly what this application's own script writes -- a shadow ID
/// that is not a GUID, a link that is not one of its temp-folder links, a letter that is not one
/// letter -- is ignored, so a cleanup never reaches anything the application did not create.
class StopLeftoverTracker {
public:
    /// True when @p line was a leftover marker (it is not script output for the log).
    bool observe(const QString& line);
    /// Reads every line of @p output.
    void observeAll(const QString& output);
    /// What was created and not removed again, in the order it was created.
    [[nodiscard]] const QVector<StopLeftover>& outstanding() const noexcept;

private:
    QVector<StopLeftover> m_outstanding;
};

/// PowerShell that removes exactly @p leftovers and reports on each one: a link before the
/// shadow copy it points into, a shadow copy only by its own ID, and cipher's folder only when it
/// is a plain folder of plain files. Nothing else is listed or removed.
[[nodiscard]] QString stopCleanupScript(const QVector<StopLeftover>& leftovers);

/// What a cleanup removed and what it could not, each named, the second with why.
struct StopCleanupReport {
    QStringList removed;
    QStringList not_removed;

    [[nodiscard]] bool isEmpty() const noexcept {
        return removed.isEmpty() && not_removed.isEmpty();
    }
};

/// Reads the cleanup script's @p output for @p leftovers. One it did not report on is listed as
/// not removed, never as removed; one that was already gone is in neither list.
[[nodiscard]] StopCleanupReport stopCleanupReport(const QVector<StopLeftover>& leftovers,
                                                  const QString& output);

/// The report when the cleanup could not run at all, for @p why.
[[nodiscard]] StopCleanupReport stopCleanupNotRun(const QVector<StopLeftover>& leftovers,
                                                  const QString& why);

/// The leftover named for a person: "the shadow copy {...}".
[[nodiscard]] QString describeStopLeftover(const StopLeftover& leftover);

/// One or two sentences for a result: "Removed after the stop: ... Not removed after the stop:
/// ... (why)." Empty when the report is.
[[nodiscard]] QString stopCleanupSummary(const StopCleanupReport& report);

/// Whether @p line is one a script writes for the application rather than for a person: a
/// Windows-continued phase marker, a cleanup marker or a cleanup result, well formed or not.
[[nodiscard]] bool isScriptMarkerLine(const QString& line);

/// Remove the drive letters still mapped to a shadow copy in this logon session (a backup's
/// mapping, DefineDosDevice, that a killed run left behind). The elevated helper calls it at start.
/// Returns how many it removed.
int removeStaleShadowMappings();

/// Pure: whether @p target, a letter's QueryDosDevice answer, is a shadow copy device.
[[nodiscard]] bool isShadowCopyDevice(const QString& target);

/// @p output as a person reads it -- in the Log window, a result, a report or a saved log --
/// without the lines isScriptMarkerLine picks out. They are read before this, never after.
[[nodiscard]] QString withoutScriptMarkers(const QString& output);

}  // namespace sak
