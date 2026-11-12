// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_stop_cleanup.cpp
/// @brief Markers for what a killed step leaves behind, and the cleanup that removes exactly that.

#include "sak/partition_stop_cleanup.h"

#include "sak/partition_script_builder.h"
#include "sak/windows_continued_work.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <string>

#include <windows.h>

namespace sak {

namespace {

constexpr auto kCleanupPrefix = "SAK-CLEANUP";
constexpr auto kResultPrefix = "SAK-CLEANUP-RESULT";
constexpr auto kCreatedField = "CREATED";
constexpr auto kReleasedField = "RELEASED";
constexpr auto kRemovedState = "REMOVED";
constexpr auto kKeptState = "KEPT";
// SAK-CLEANUP|<CREATED|RELEASED>|<kind>|<value>
constexpr qsizetype kMarkerFieldCount = 4;
// SAK-CLEANUP-RESULT|<REMOVED|KEPT|GONE>|<kind>|<value>|<why>; why may itself be cut by '|'.
constexpr qsizetype kResultMinimumFieldCount = 5;
constexpr qsizetype kStateField = 1;
constexpr qsizetype kKindField = 2;
constexpr qsizetype kValueField = 3;
constexpr qsizetype kWhyField = 4;

QString kindToken(StopLeftoverKind kind) {
    switch (kind) {
    case StopLeftoverKind::ShadowCopy:
        return QStringLiteral("SHADOW");
    case StopLeftoverKind::ShadowLink:
        return QStringLiteral("LINK");
    case StopLeftoverKind::CipherFolder:
        return QStringLiteral("CIPHER");
    }
    return QStringLiteral("SHADOW");
}

std::optional<StopLeftoverKind> kindForToken(const QString& token) {
    for (const StopLeftoverKind kind : {StopLeftoverKind::ShadowCopy,
                                        StopLeftoverKind::ShadowLink,
                                        StopLeftoverKind::CipherFolder}) {
        if (kindToken(kind) == token) {
            return kind;
        }
    }
    return std::nullopt;
}

// Exactly what the application's own scripts write, and nothing a cleanup could be turned toward:
// Win32_ShadowCopy.Create's "{GUID}", "<letter>=\Device\HarddiskVolumeShadowCopy<n>" for the
// drive letter mapped to it, and one drive letter.
bool valueIsOurs(StopLeftoverKind kind, const QString& value) {
    static const QRegularExpression kShadowId(QStringLiteral(
        "^\\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\}$"));
    static const QRegularExpression kShadowLink(
        QStringLiteral("^[A-Za-z]=\\\\Device\\\\HarddiskVolumeShadowCopy[0-9]+$"));
    static const QRegularExpression kDriveLetter(QStringLiteral("^[A-Za-z]$"));
    switch (kind) {
    case StopLeftoverKind::ShadowCopy:
        return kShadowId.match(value).hasMatch();
    case StopLeftoverKind::ShadowLink:
        return kShadowLink.match(value).hasMatch();
    case StopLeftoverKind::CipherFolder:
        return kDriveLetter.match(value).hasMatch();
    }
    return false;
}

QString markerStatement(const char* field, StopLeftoverKind kind, const QString& value_expression) {
    return QStringLiteral("[Console]::Out.WriteLine('%1|%2|%3|' + (%4)); [Console]::Out.Flush()")
        .arg(
            QLatin1String(kCleanupPrefix), QLatin1String(field), kindToken(kind), value_expression);
}

QString powerShellLiteral(const QString& value) {
    return QLatin1Char('\'') + QString(value).replace(QLatin1Char('\''), QStringLiteral("''")) +
           QLatin1Char('\'');
}

// The functions the cleanup calls, each writing one result line for what it was given.
QString stopCleanupFunctionsScript() {
    return PartitionScriptBuilder::scriptTypesLoaderScript() +
           QStringLiteral(
               "$ErrorActionPreference = 'Stop'\n"
               "function Write-SakCleanupResult([string]$state, [string]$kind, [string]$what, "
               "[string]$why) {\n"
               "  [Console]::Out.WriteLine(('SAK-CLEANUP-RESULT|{0}|{1}|{2}|{3}' -f $state, $kind, "
               "$what, ($why -replace '[\\r\\n|]+', ' ')))\n"
               "  [Console]::Out.Flush()\n"
               "}\n"
               "function Remove-SakShadowLink([string]$mapping) {\n"
               "  try {\n"
               "    $letter, $device = $mapping -split '=', 2\n"
               "    if (-not [Sak.ShadowMapping]::Unmap(($letter + ':'), $device)) { "
               "Write-SakCleanupResult 'GONE' 'LINK' $mapping ''; return }\n"
               "    Write-SakCleanupResult 'REMOVED' 'LINK' $mapping ''\n"
               "  } catch { Write-SakCleanupResult 'KEPT' 'LINK' $mapping $_.Exception.Message }\n"
               "}\n"
               "function Remove-SakShadowCopy([string]$id) {\n"
               "  try {\n"
               "    $shadow = Get-CimInstance Win32_ShadowCopy -Filter (\"ID='{0}'\" -f $id) "
               "-ErrorAction Stop\n"
               "    if (-not $shadow) { Write-SakCleanupResult 'GONE' 'SHADOW' $id ''; return }\n"
               "    Remove-CimInstance -InputObject $shadow -ErrorAction Stop\n"
               "    Write-SakCleanupResult 'REMOVED' 'SHADOW' $id ''\n"
               "  } catch { Write-SakCleanupResult 'KEPT' 'SHADOW' $id $_.Exception.Message }\n"
               "}\n"
               "function Remove-SakCipherFolder([string]$letter) {\n"
               "  $folder = ('{0}:\\EFSTMPWP' -f $letter)\n"
               "  try {\n"
               "    $item = Get-Item -LiteralPath $folder -Force -ErrorAction SilentlyContinue\n"
               "    if (-not $item) { Write-SakCleanupResult 'GONE' 'CIPHER' $letter ''; return }\n"
               "    if (-not $item.PSIsContainer -or ($item.Attributes -band "
               "[System.IO.FileAttributes]::ReparsePoint)) { Write-SakCleanupResult 'KEPT' "
               "'CIPHER' "
               "$letter 'it is not a plain folder'; return }\n"
               "    $inside = @(Get-ChildItem -LiteralPath $folder -Force -ErrorAction Stop)\n"
               "    if (@($inside | Where-Object { $_.PSIsContainer -or ($_.Attributes -band "
               "[System.IO.FileAttributes]::ReparsePoint) }).Count -gt 0) { Write-SakCleanupResult "
               "'KEPT' 'CIPHER' $letter 'it holds a folder or a link, which cipher does not "
               "create'; "
               "return }\n"
               "    foreach ($file in $inside) { [System.IO.File]::Delete($file.FullName) }\n"
               "    [System.IO.Directory]::Delete($folder)\n"
               "    Write-SakCleanupResult 'REMOVED' 'CIPHER' $letter ''\n"
               "  } catch { Write-SakCleanupResult 'KEPT' 'CIPHER' $letter $_.Exception.Message }\n"
               "}\n");
}

QString cleanupCall(const StopLeftover& leftover) {
    switch (leftover.kind) {
    case StopLeftoverKind::ShadowCopy:
        return QStringLiteral("Remove-SakShadowCopy %1\n").arg(powerShellLiteral(leftover.value));
    case StopLeftoverKind::ShadowLink:
        return QStringLiteral("Remove-SakShadowLink %1\n").arg(powerShellLiteral(leftover.value));
    case StopLeftoverKind::CipherFolder:
        return QStringLiteral("Remove-SakCipherFolder %1\n").arg(powerShellLiteral(leftover.value));
    }
    return {};
}

struct CleanupResult {
    QString state;
    QString why;
};

QHash<QString, CleanupResult> cleanupResults(const QString& output) {
    QHash<QString, CleanupResult> results;
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QStringList fields = line.trimmed().split(QLatin1Char('|'));
        if (fields.size() < kResultMinimumFieldCount ||
            fields.constFirst() != QLatin1String(kResultPrefix)) {
            continue;
        }
        const QString key = fields.at(kKindField) + QLatin1Char('|') + fields.at(kValueField);
        results.insert(key,
                       {.state = fields.at(kStateField),
                        .why = fields.mid(kWhyField).join(QLatin1Char(' ')).trimmed()});
    }
    return results;
}

}  // namespace

QString stopLeftoverCreatedStatement(StopLeftoverKind kind, const QString& value_expression) {
    return markerStatement(kCreatedField, kind, value_expression);
}

QString stopLeftoverReleasedStatement(StopLeftoverKind kind, const QString& value_expression) {
    return markerStatement(kReleasedField, kind, value_expression);
}

bool StopLeftoverTracker::observe(const QString& line) {
    const QStringList fields = line.trimmed().split(QLatin1Char('|'));
    if (fields.size() != kMarkerFieldCount ||
        fields.constFirst() != QLatin1String(kCleanupPrefix)) {
        return false;
    }
    const auto kind = kindForToken(fields.at(kKindField));
    const QString& value = fields.at(kValueField);
    if (!kind || !valueIsOurs(*kind, value)) {
        return true;  // A marker, but not one naming something this application made.
    }
    const StopLeftover leftover{.kind = *kind, .value = value};
    if (fields.at(kStateField) == QLatin1String(kCreatedField)) {
        if (!m_outstanding.contains(leftover)) {
            m_outstanding.append(leftover);
        }
    } else if (fields.at(kStateField) == QLatin1String(kReleasedField)) {
        m_outstanding.removeAll(leftover);
    }
    return true;
}

void StopLeftoverTracker::observeAll(const QString& output) {
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        observe(line);
    }
}

const QVector<StopLeftover>& StopLeftoverTracker::outstanding() const noexcept {
    return m_outstanding;
}

QString stopCleanupScript(const QVector<StopLeftover>& leftovers) {
    QVector<StopLeftover> ordered;
    for (const StopLeftover& leftover : leftovers) {
        if (valueIsOurs(leftover.kind, leftover.value)) {
            ordered.append(leftover);
        }
    }
    // A link is removed before the shadow copy it points into.
    std::ranges::stable_partition(ordered, [](const StopLeftover& leftover) {
        return leftover.kind == StopLeftoverKind::ShadowLink;
    });
    QString script = stopCleanupFunctionsScript();
    for (const StopLeftover& leftover : ordered) {
        script += cleanupCall(leftover);
    }
    return script;
}

StopCleanupReport stopCleanupReport(const QVector<StopLeftover>& leftovers, const QString& output) {
    const QHash<QString, CleanupResult> results = cleanupResults(output);
    StopCleanupReport report;
    for (const StopLeftover& leftover : leftovers) {
        const QString what = describeStopLeftover(leftover);
        const auto found =
            results.constFind(kindToken(leftover.kind) + QLatin1Char('|') + leftover.value);
        if (found == results.constEnd()) {
            report.not_removed.append(
                QStringLiteral("%1 (the cleanup did not report on it)").arg(what));
        } else if (found->state == QLatin1String(kRemovedState)) {
            report.removed.append(what);
        } else if (found->state == QLatin1String(kKeptState)) {
            report.not_removed.append(QStringLiteral("%1 (%2)").arg(what, found->why));
        }
    }
    return report;
}

StopCleanupReport stopCleanupNotRun(const QVector<StopLeftover>& leftovers, const QString& why) {
    StopCleanupReport report;
    for (const StopLeftover& leftover : leftovers) {
        report.not_removed.append(
            QStringLiteral("%1 (%2)").arg(describeStopLeftover(leftover), why));
    }
    return report;
}

QString describeStopLeftover(const StopLeftover& leftover) {
    switch (leftover.kind) {
    case StopLeftoverKind::ShadowCopy:
        return QStringLiteral("the shadow copy %1").arg(leftover.value);
    case StopLeftoverKind::ShadowLink:
        return QStringLiteral("the drive letter mapped to the shadow copy (%1)")
            .arg(leftover.value);
    case StopLeftoverKind::CipherFolder:
        return QStringLiteral("cipher's temporary folder %1:\\EFSTMPWP").arg(leftover.value);
    }
    return leftover.value;
}

bool isShadowCopyDevice(const QString& target) {
    static const QRegularExpression kShadowDevice(
        QStringLiteral("^\\\\Device\\\\HarddiskVolumeShadowCopy[0-9]+$"),
        QRegularExpression::CaseInsensitiveOption);
    return kShadowDevice.match(target).hasMatch();
}

int removeStaleShadowMappings() {
    constexpr DWORD kTargetChars = 1024;
    int removed = 0;
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        const std::wstring device{letter, L':'};
        std::wstring target(kTargetChars, L'\0');
        const DWORD length = QueryDosDeviceW(device.c_str(), target.data(), kTargetChars);
        if (length == 0) {
            continue;
        }
        const QString first = QString::fromWCharArray(target.c_str());  // up to the first NUL
        if (!isShadowCopyDevice(first)) {
            continue;
        }
        if (DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION |
                                 DDD_EXACT_MATCH_ON_REMOVE,
                             device.c_str(),
                             reinterpret_cast<LPCWSTR>(first.utf16())) != 0) {
            ++removed;
        }
    }
    return removed;
}

bool isScriptMarkerLine(const QString& line) {
    const QString trimmed = line.trimmed();
    return isWindowsContinuedMarkerLine(trimmed) ||
           trimmed.startsWith(QLatin1String(kCleanupPrefix) + QLatin1Char('|')) ||
           trimmed.startsWith(QLatin1String(kResultPrefix) + QLatin1Char('|'));
}

namespace {

// A run of blank lines and marker lines between two lines of text. PowerShell's formatter puts
// blank lines on both sides of a marker written while it holds a table back, so dropping the
// markers alone left their blank lines stacked up. The blank lines of a run that held a marker
// collapse to the longest stretch the script itself wrote between markers; a run with no marker
// in it is the script's own and is kept as it was.
struct BlankRun {
    QString blank_line;        ///< One of the run's blank lines, as written ("\r" or "").
    qsizetype blank_lines{0};  ///< Every blank line in the run.
    qsizetype stretch{0};      ///< Blank lines since the last marker.
    qsizetype longest_stretch{0};
    bool held_marker{false};

    void add(const QString& line) {
        if (isScriptMarkerLine(line)) {
            held_marker = true;
            longest_stretch = std::max(longest_stretch, stretch);
            stretch = 0;
            return;
        }
        blank_line = line;
        ++blank_lines;
        ++stretch;
    }

    void flushInto(QStringList* kept) {
        const qsizetype keep = held_marker ? std::max(longest_stretch, stretch) : blank_lines;
        for (qsizetype i = 0; i < keep; ++i) {
            kept->append(blank_line);
        }
        *this = {};
    }
};

}  // namespace

QString withoutScriptMarkers(const QString& output) {
    QStringList lines = output.split(QLatin1Char('\n'));
    // The empty piece after a final newline is not a line; the join puts the newline back.
    const bool ends_with_newline = lines.size() > 1 && lines.constLast().isEmpty();
    if (ends_with_newline) {
        lines.removeLast();
    }
    QStringList kept;
    BlankRun run;
    for (const QString& line : lines) {
        if (isScriptMarkerLine(line) || line.trimmed().isEmpty()) {
            run.add(line);
            continue;
        }
        run.flushInto(&kept);
        kept.append(line);
    }
    run.flushInto(&kept);
    if (ends_with_newline) {
        kept.append(QString());
    }
    return kept.join(QLatin1Char('\n'));
}

QString stopCleanupSummary(const StopCleanupReport& report) {
    QStringList sentences;
    if (!report.removed.isEmpty()) {
        sentences.append(QStringLiteral("Removed after the stop: %1.")
                             .arg(report.removed.join(QStringLiteral("; "))));
    }
    if (!report.not_removed.isEmpty()) {
        sentences.append(QStringLiteral("Not removed after the stop: %1.")
                             .arg(report.not_removed.join(QStringLiteral("; "))));
    }
    return sentences.join(QLatin1Char(' '));
}

}  // namespace sak
