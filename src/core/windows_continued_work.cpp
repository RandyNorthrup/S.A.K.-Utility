// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows_continued_work.cpp
/// @brief Markers and wording for steps whose work Windows finishes on its own.

#include "sak/windows_continued_work.h"

#include "sak/partition_manager_types.h"

#include <QLocale>
#include <QStringList>

#include <array>
#include <cstddef>

namespace sak {

namespace {

constexpr auto kMarkerPrefix = "SAK-WINDOWS-CONTINUES";
constexpr auto kBeginField = "BEGIN";
constexpr auto kEndField = "END";
// SAK-WINDOWS-CONTINUES|BEGIN|<work>|<bytes> and SAK-WINDOWS-CONTINUES|END|<work>.
constexpr qsizetype kBeginFieldCount = 4;
constexpr qsizetype kEndFieldCount = 3;
constexpr qsizetype kKindField = 1;
constexpr qsizetype kWorkField = 2;
constexpr qsizetype kSizeField = 3;

struct WorkName {
    WindowsContinuedWork work;
    const char* token;
    const char* text;
};

// In enumerator order, so a work's entry is found by its value.
constexpr auto kWorkNames = std::to_array<WorkName>({
    {WindowsContinuedWork::FullFormat, "full-format", "a full format"},
    {WindowsContinuedWork::Defragment, "defragment", "a defragmentation"},
    {WindowsContinuedWork::Retrim, "retrim", "an SSD ReTrim"},
    {WindowsContinuedWork::PartitionShrink, "partition-shrink", "a partition shrink"},
    {WindowsContinuedWork::VolumeScan, "volume-scan", "a file system scan"},
});

constexpr bool workNamesInEnumeratorOrder() {
    for (std::size_t index = 0; index < kWorkNames.size(); ++index) {
        if (static_cast<std::size_t>(kWorkNames.at(index).work) != index) {
            return false;
        }
    }
    return kWorkNames.size() == static_cast<std::size_t>(WindowsContinuedWork::VolumeScan) + 1;
}
static_assert(workNamesInEnumeratorOrder(), "every WindowsContinuedWork has its entry, in order");

const WorkName& workName(WindowsContinuedWork work) {
    return kWorkNames.at(static_cast<std::size_t>(work));
}

std::optional<WindowsContinuedWork> workForToken(const QString& token) {
    for (const WorkName& name : kWorkNames) {
        if (token == QLatin1String(name.token)) {
            return name.work;
        }
    }
    return std::nullopt;
}

}  // namespace

QString windowsContinuedStatement(WindowsContinuedWork work,
                                  const QString& size_expression,
                                  const QString& statement) {
    const QString prefix = QLatin1String(kMarkerPrefix);
    const QString token = QLatin1String(workName(work).token);
    // One line, so a guard that addDiskIdentityGuards puts in front of a disk-mutating line goes
    // in front of the Begin marker too, and a refused guard never leaves a phase open.
    return QStringLiteral(
               "[Console]::Out.WriteLine(('%1|BEGIN|%2|{0}' -f [uint64](%3))); "
               "[Console]::Out.Flush(); try { %4 } finally { "
               "[Console]::Out.WriteLine('%1|END|%2'); [Console]::Out.Flush() }")
        .arg(prefix, token, size_expression, statement);
}

bool isWindowsContinuedMarkerLine(const QString& line) {
    return line.trimmed().startsWith(QLatin1String(kMarkerPrefix) + QLatin1Char('|'));
}

std::optional<WindowsContinuedMarker> parseWindowsContinuedMarker(const QString& line) {
    const QStringList fields = line.trimmed().split(QLatin1Char('|'));
    if (fields.size() < kEndFieldCount || fields.constFirst() != QLatin1String(kMarkerPrefix)) {
        return std::nullopt;
    }
    const auto work = workForToken(fields.at(kWorkField));
    if (!work) {
        return std::nullopt;
    }
    WindowsContinuedMarker marker;
    marker.work = *work;
    if (fields.at(kKindField) == QLatin1String(kEndField) && fields.size() == kEndFieldCount) {
        return marker;
    }
    if (fields.at(kKindField) != QLatin1String(kBeginField) || fields.size() != kBeginFieldCount) {
        return std::nullopt;
    }
    marker.begin = true;
    // The size only describes the work; a value the script could not produce reads as unknown.
    bool ok = false;
    const qulonglong bytes = fields.at(kSizeField).toULongLong(&ok);
    marker.size_bytes = ok ? bytes : 0;
    return marker;
}

std::optional<WindowsContinuedMarker> openWindowsContinuedPhase(const QString& output) {
    std::optional<WindowsContinuedMarker> open;
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const auto marker = parseWindowsContinuedMarker(line);
        if (!marker) {
            continue;
        }
        if (marker->begin) {
            open = marker;
        } else {
            open.reset();
        }
    }
    return open;
}

QString windowsContinuedWorkText(WindowsContinuedWork work) {
    return QLatin1String(workName(work).text);
}

QString windowsContinuedWorkDescription(const WindowsContinuedMarker& marker) {
    const QString work = windowsContinuedWorkText(marker.work);
    return marker.size_bytes == 0
               ? work
               : QStringLiteral("%1 (%2)").arg(work, formatPartitionBytes(marker.size_bytes));
}

QString windowsContinuedPhaseNotice(const WindowsContinuedPhase& phase) {
    return QStringLiteral(
               "%1: Windows is carrying out %2. This step cannot be stopped once it has started, "
               "and Windows continues it even if S.A.K. Utility is closed. Started at %3. No time "
               "estimate is available.")
        .arg(phase.step_summary,
             windowsContinuedWorkDescription(phase.marker),
             QLocale().toString(phase.started_at.time(), QLocale::ShortFormat));
}

QString windowsContinuedExitNote(const WindowsContinuedPhase& phase) {
    return QStringLiteral(
               "%1: Windows is carrying out %2. Closing S.A.K. Utility will not stop it; Windows "
               "continues it. If you exit now, the rest of this step and anything queued after it "
               "do not run.")
        .arg(phase.step_summary, windowsContinuedWorkDescription(phase.marker));
}

QString windowsContinuedStepMessage(const QString& step_summary,
                                    const WindowsContinuedMarker& marker) {
    return QStringLiteral(
               "%1 may still be running in Windows: its script was stopped while Windows was "
               "carrying out %2, which Windows finishes on its own. The rest of this step did "
               "not run.")
        .arg(step_summary, windowsContinuedWorkDescription(marker));
}

}  // namespace sak
