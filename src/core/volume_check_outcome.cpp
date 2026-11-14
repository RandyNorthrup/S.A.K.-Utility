// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file volume_check_outcome.cpp
/// @brief What a read-only check of a volume or file system found, in plain words.

#include "sak/volume_check_outcome.h"

#include "sak/format_utils.h"
#include "sak/partition_file_system_tool_runner.h"

#include <QRegularExpression>
#include <QStringList>

namespace sak::volume_check {

namespace {

/// How many candidate boot sectors a recovery scan's result names; the log has every one.
constexpr qsizetype kRecoveryCandidatesShown = 10;

/// "Drive E: <said>."
QString driveSentence(const QString& letter, const QString& said) {
    return QStringLiteral("Drive %1: %2.").arg(letter, said);
}

Outcome onlineScanOutcome(const QString& letter, const QString& status) {
    switch (repairStatusFinding(status)) {
    case Finding::NoErrors:
        return {.text = driveSentence(letter, noErrorsText()), .needs_attention = false};
    case Finding::FixedOnline:
        return {.text = driveSentence(letter, fixedOnlineText()), .needs_attention = false};
    case Finding::ErrorsFound:
        return {.text = driveSentence(letter, windowsReportsErrorsText(status)) + QLatin1Char(' ') +
                        repairRecommendedText() + QLatin1Char('.'),
                .needs_attention = true};
    case Finding::NotChecked:
        break;
    }
    return {.text =
                driveSentence(letter,
                              QStringLiteral("Not checked (Windows answered \"%1\", which is not "
                                             "an answer about the drive)")
                                  .arg(status)),
            .needs_attention = true};
}

Outcome readOnlyChkdskOutcome(const QString& letter, const QString& verdict) {
    if (verdict == QStringLiteral("None")) {
        return {.text = driveSentence(letter,
                                      noErrorsText() + QStringLiteral(" (read-only chkdsk)")),
                .needs_attention = false};
    }
    // Problems found, or no check at all, stop the script with their own words before this line.
    return {.text = driveSentence(
                letter,
                QStringLiteral("Not checked (the read-only chkdsk answered \"%1\")").arg(verdict)),
            .needs_attention = true};
}

/// Why a read-only tool check did not pass, without the tool's name the script puts first
/// ("e2fsck: e2fsck found errors ...").
QString toolFailureDetail(const PartitionFileSystemToolRunResult& result, const QString& tool) {
    QString problem = result.problem.trimmed();
    const QString prefix = tool + QStringLiteral(": ");
    if (problem.startsWith(prefix)) {
        problem = problem.mid(prefix.size());
    }
    if (!problem.isEmpty()) {
        return problem;
    }
    static const QRegularExpression kExitLine(
        QStringLiteral(R"(Filesystem tool exit code: (-?\d+))"));
    const auto exit_line = kExitLine.match(result.std_out);
    return QStringLiteral("%1 ended with exit code %2.")
        .arg(tool,
             exit_line.hasMatch() ? exit_line.captured(1) : QString::number(result.exit_code));
}

/// fsck_hfs -n says a damaged volume "was found corrupt and needs to be repaired".
bool fsckHfsFoundErrors(const QString& output) {
    static const QRegularExpression kCorrupt(QStringLiteral(
                                                 R"(found corrupt|needs to be repaired)"),
                                             QRegularExpression::CaseInsensitiveOption);
    return kCorrupt.match(output).hasMatch();
}

}  // namespace

Finding repairStatusFinding(const QString& repair_status) {
    static const QStringList kClean = {QStringLiteral("NoErrorsFound"),
                                       QStringLiteral("ScanNoErrorsFound")};
    static const QStringList kFixed = {QStringLiteral("ErrorsFixed"),
                                       QStringLiteral("MinorErrorsFixedOrCleanup"),
                                       QStringLiteral("ScanErrorsFoundAndFixedOnline"),
                                       QStringLiteral("ScanFoundAndFixedLeaks")};
    static const QStringList kNeedsRepair = {QStringLiteral("ScanErrorsFixedOnlineAlsoNeedSpotFix"),
                                             QStringLiteral("ScanErrorsFoundNeedSpotFix"),
                                             QStringLiteral("NeedOfflineScanAndFix"),
                                             QStringLiteral("ScanFoundLeaks")};
    if (kClean.contains(repair_status)) {
        return Finding::NoErrors;
    }
    if (kFixed.contains(repair_status)) {
        return Finding::FixedOnline;
    }
    if (kNeedsRepair.contains(repair_status)) {
        return Finding::ErrorsFound;
    }
    return Finding::NotChecked;
}

QString noErrorsText() {
    return QStringLiteral("No errors found");
}

QString fixedOnlineText() {
    return QStringLiteral("Errors found and fixed by Windows while the drive stayed online");
}

QString windowsReportsErrorsText(const QString& repair_status) {
    return QStringLiteral("Errors found (Windows reports %1)").arg(repair_status);
}

QString readOnlyErrorsText(const QString& check) {
    return QStringLiteral("Errors found by a read-only %1, which fixed none of them").arg(check);
}

QString repairRecommendedText() {
    return QStringLiteral("Repair recommended (this check does not modify the disk)");
}

QString verdictLineStatement(QLatin1StringView route, const QString& variable) {
    return QStringLiteral("Write-Output ('Volume check {0}: %1: {1}' -f $sakScanDrive, [string]%2)")
        .arg(QString(route), variable);
}

QVector<Outcome> volumeOutcomes(const QString& script_output) {
    static const QRegularExpression kVerdictLine(
        QStringLiteral(R"(\bVolume check ([A-Za-z]): (online scan|read-only chkdsk): (\S*)\s*$)"),
        QRegularExpression::MultilineOption);
    QVector<Outcome> outcomes;
    auto lines = kVerdictLine.globalMatch(script_output);
    while (lines.hasNext()) {
        const auto line = lines.next();
        const QString letter = line.captured(1).toUpper();
        outcomes.append(line.captured(2) == kOnlineScanRoute
                            ? onlineScanOutcome(letter, line.captured(3))
                            : readOnlyChkdskOutcome(letter, line.captured(3)));
    }
    return outcomes;
}

Outcome recoveryScanOutcome(uint32_t disk_number, const QString& script_output) {
    static const QRegularExpression kCandidate(
        QStringLiteral(R"(Candidate partition boot sector at byte offset [^\r\n]*)"));
    QStringList candidates;
    auto found = kCandidate.globalMatch(script_output);
    while (found.hasNext()) {
        candidates.append(found.next().captured(0).trimmed());
    }
    if (candidates.isEmpty()) {
        return {.text = QStringLiteral("The recovery scan of Disk %1 found no lost partition.")
                            .arg(disk_number),
                .needs_attention = false};
    }
    QString text = QStringLiteral("The recovery scan of Disk %1 found %2:\n%3")
                       .arg(QString::number(disk_number),
                            countOf(candidates.size(),
                                    QStringLiteral("possible lost partition"),
                                    QStringLiteral("possible lost partitions")),
                            candidates.mid(0, kRecoveryCandidatesShown).join(QLatin1Char('\n')));
    if (candidates.size() > kRecoveryCandidatesShown) {
        text += QStringLiteral("\nand %1 more; the log has every one.")
                    .arg(candidates.size() - kRecoveryCandidatesShown);
    }
    return {.text = text, .needs_attention = false};
}

Outcome toolCheckOutcome(const PartitionFileSystemToolRunResult& result,
                         const QString& file_system) {
    const QString tool = result.tool_id.trimmed().isEmpty() ? QStringLiteral("check")
                                                            : result.tool_id.trimmed();
    const QString subject = QStringLiteral("The %1 file system").arg(file_system.trimmed());
    if (result.cancelled) {
        return {.text = subject + QStringLiteral(": Not checked; the check was cancelled, and "
                                                 "nothing was changed."),
                .needs_attention = false};
    }
    if (result.blocked) {
        return {.text = subject + QStringLiteral(": Not checked. ") +
                        result.blockers.join(QLatin1Char(' ')),
                .needs_attention = true};
    }
    if (result.success) {
        return {.text = QStringLiteral("%1: %2 (read-only %3).").arg(subject, noErrorsText(), tool),
                .needs_attention = false};
    }
    const QString detail = toolFailureDetail(result, tool);
    if (PartitionFileSystemToolRunner::problemIsAFinding(detail) ||
        fsckHfsFoundErrors(result.std_out)) {
        return {.text = QStringLiteral("%1: %2. %3").arg(subject, readOnlyErrorsText(tool), detail),
                .needs_attention = true};
    }
    return {.text = QStringLiteral("%1: Not checked. %2").arg(subject, detail),
            .needs_attention = true};
}

}  // namespace sak::volume_check
