// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file restore_point_manager.cpp
/// @brief System Restore point creation and availability checking

#include "sak/restore_point_manager.h"

#include "sak/elevation_manager.h"
#include "sak/process_runner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

RestorePointManager::RestorePointManager(QObject* parent) : QObject(parent) {}

bool RestorePointManager::isSystemRestoreEnabled() const {
    // Authoritative check: Enable-ComputerRestore sets RPSessionInterval to 1 and
    // Disable-ComputerRestore sets it to 0, so RPSessionInterval > 0 iff System
    // Protection is on. Do NOT fall back to the VSS service state -- VSS runs for
    // shadow copies/backups independently of System Restore, so a running VSS
    // would falsely report "enabled" (a safety-check failing OPEN, letting a
    // destructive op skip its restore point).
    //
    // Launch the System32-qualified interpreter, never a bare "powershell.exe": this
    // manager runs elevated, so a PATH/CWD-planted powershell would execute with our
    // token. Unresolvable -> report NOT enabled (fail closed), never probe via PATH.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        return false;
    }

    const auto result = sak::runProcess(
        powershell,
        {QStringLiteral("-NoProfile"),
         QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"),
         QStringLiteral(
             "try { "
             "  $k = Get-ItemProperty "
             "    -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore' "
             "    -Name 'RPSessionInterval' -ErrorAction Stop; "
             // Require the REG_DWORD the API documents. A [int] cast would coerce a planted
             // string ("1") or any other type into a number, letting a wrong-typed registry
             // value report System Protection as on -- a safety check failing OPEN.
             "  if (($k.RPSessionInterval -is [int]) -and ($k.RPSessionInterval -gt 0)) "
             "    { Write-Output 'ENABLED'; } "
             "  else { Write-Output 'DISABLED'; } "
             "} catch { "
             "  Write-Output 'DISABLED'; "  // fail closed: cannot confirm -> not enabled
             "}")},
        kCheckTimeoutMs);

    return restoreEnabledFromProbe(result.succeeded(), result.std_out);
}

bool RestorePointManager::restoreEnabledFromProbe(bool probeSucceeded, const QString& output) {
    // Fail closed: only a successful probe that explicitly reports ENABLED counts.
    return probeSucceeded && output.trimmed() == QStringLiteral("ENABLED");
}

namespace {

/// @brief Checkpoint-Computer script for @p safe_desc (single quotes already doubled).
///
/// Checkpoint-Computer does NOT throw when Windows SKIPS creation under the once-per-24h
/// frequency throttle -- it only writes a warning (and exits 0). The script fails on that
/// warning so a silent no-op is never reported as a created restore point; the warning's
/// '1440'/'frequency' text then routes to the throttle branch in the caller.
QString buildCheckpointScript(const QString& safe_desc) {
    return QString(
               "try { "
               "  Checkpoint-Computer -Description '%1' "
               "    -RestorePointType 'APPLICATION_UNINSTALL' "
               "    -ErrorAction Stop "
               "    -WarningVariable wv "
               "    -WarningAction SilentlyContinue; "
               "  if ($wv) { Write-Error $wv; exit 1; } "
               "  Write-Output 'SUCCESS'; "
               "} catch { "
               "  Write-Error $_.Exception.Message; "
               "  exit 1; "
               "}")
        .arg(safe_desc);
}

/// @brief The detail worth reporting from a checkpoint run: stderr, or stdout when stderr is bare.
QString checkpointErrorDetail(const sak::ProcessResult& result) {
    const QString err = result.std_err.trimmed();
    return err.isEmpty() ? result.std_out.trimmed() : err;
}

/// @brief Why @p result is not a created restore point; an empty string means it is one.
///
/// Exit 0 alone is not proof of a checkpoint: require the script's explicit SUCCESS sentinel, so a
/// run that exited clean without reaching the Write-Output (truncated/garbled output, a host that
/// swallowed the error) is never reported as a created restore point. Downstream destructive work
/// asks this question before it starts, so it must fail closed. Every failure reason is non-empty,
/// so the empty string is unambiguously the success answer.
QString checkpointFailureReason(const sak::ProcessResult& result) {
    if (result.timed_out) {
        return QStringLiteral("Timeout creating restore point (exceeded 2 minutes).");
    }

    if (result.exit_code != 0) {
        const QString err = checkpointErrorDetail(result);

        // Check for throttle (Windows only allows one per 24h in some configs)
        if (err.contains("frequency", Qt::CaseInsensitive) ||
            err.contains("1440", Qt::CaseInsensitive)) {
            return QStringLiteral(
                "Windows limits restore point creation to once per 24 hours. "
                "A recent restore point already exists.");
        }
        return QString("Failed to create restore point: %1").arg(err);
    }

    if (!result.std_out.contains(QStringLiteral("SUCCESS"))) {
        return QString("Restore point creation did not confirm success: %1")
            .arg(checkpointErrorDetail(result));
    }

    return {};
}

}  // namespace

QString RestorePointManager::restorePointPreflightRefusal(const QString& description,
                                                          bool elevated) {
    // Reject an empty description at runtime rather than assert it: an empty -Description is not a
    // checkpoint Windows will label usefully, and the caller gets the same honest failure in both
    // build configurations.
    if (description.isEmpty()) {
        return QStringLiteral("A restore point description is required.");
    }
    // Creating a restore point is an elevated operation; a non-elevated process must be refused
    // here rather than attempting it and failing deep inside the elevated step.
    if (!elevated) {
        return QStringLiteral("Creating restore points requires administrator privileges.");
    }
    return {};
}

bool RestorePointManager::createRestorePoint(const QString& description) {
    // The empty-description and administrator-privileges preflight is extracted into the pure,
    // side-effect-free restorePointPreflightRefusal so it is unit-testable without invoking the
    // real elevated work; here it runs against the live elevation state.
    const QString refusal = restorePointPreflightRefusal(description, isElevated());
    if (!refusal.isEmpty()) {
        Q_EMIT restorePointFailed(refusal);
        return false;
    }

    // Truncate description to max length
    QString safe_desc = description.left(kMaxDescriptionLength);

    // Escape single quotes in description
    safe_desc.replace("'", "''");

    // Restore-point creation is the elevated step: resolve the System32 interpreter and
    // REFUSE to run if it cannot be resolved rather than let CreateProcess search PATH/CWD.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        Q_EMIT restorePointFailed(
            "Cannot resolve the System32 PowerShell path; refusing to create a restore point.");
        return false;
    }

    const auto result = sak::runProcess(powershell,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NonInteractive"),
                                         QStringLiteral("-Command"),
                                         buildCheckpointScript(safe_desc)},
                                        kCreateTimeoutMs);

    const QString failure = checkpointFailureReason(result);
    if (!failure.isEmpty()) {
        Q_EMIT restorePointFailed(failure);
        return false;
    }

    Q_EMIT restorePointCreated(safe_desc);
    return true;
}

namespace {

// The serialized form is "/Date(<epoch-ms>[+hhmm])/": the leading "/Date(" prefix is 6 chars,
// and the full wrapper (that prefix plus the trailing ")/") is 8 chars.
constexpr int kEpochDatePrefixLength = 6;
constexpr int kEpochDateWrapperLength = 8;

/// Windows PowerShell 5.1 serializes a DateTime through JavaScriptSerializer, so ConvertTo-Json
/// can emit "/Date(1754323200000)/" (epoch milliseconds, optionally with a +hhmm offset suffix)
/// rather than an ISO string. The query below asks for a round-trip ISO string, but accept this
/// documented encoding too: without it every restore point on such a host parses as invalid.
QDateTime parseEpochMillisecondsDate(const QString& date_str) {
    if (!date_str.startsWith(QLatin1String("/Date(")) || !date_str.endsWith(QLatin1String(")/"))) {
        return {};
    }
    QString digits = date_str.mid(kEpochDatePrefixLength,
                                  date_str.size() - kEpochDateWrapperLength);
    qsizetype offset_at = digits.lastIndexOf(QLatin1Char('+'));
    if (offset_at < 1) {
        offset_at = digits.lastIndexOf(QLatin1Char('-'));
    }
    if (offset_at > 0) {
        digits = digits.left(offset_at);  // the epoch value itself is UTC; the suffix is display
    }
    bool ok = false;
    const qint64 msecs = digits.toLongLong(&ok);
    if (!ok) {
        return {};
    }
    QDateTime dt;
    dt.setMSecsSinceEpoch(msecs);
    return dt;
}

QDateTime parseRestorePointDate(const QString& date_str) {
    const QString trimmed = date_str.trimmed();
    QDateTime dt = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (dt.isValid()) {
        return dt;
    }
    dt = QDateTime::fromString(trimmed, Qt::ISODate);
    if (dt.isValid()) {
        return dt;
    }
    dt = parseEpochMillisecondsDate(trimmed);
    if (dt.isValid()) {
        return dt;
    }
    return QDateTime::fromString(trimmed, "M/d/yyyy h:mm:ss AP");
}

/// @brief The Get-ComputerRestorePoint query, one compressed JSON record per restore point.
///
/// Fail closed: SilentlyContinue would swallow a WMI/query failure and emit empty output with a
/// zero exit, which reads as a genuine "no restore points". Force errors to terminate and exit
/// non-zero so a failed query is surfaced (queryOk stays false) instead of masqueraded as an
/// empty result. A true zero-restore-point machine still exits 0 with empty output.
///
/// ToString('o') pins the timestamp to a culture-invariant round-trip ISO string. Handing
/// ConvertTo-Json a raw DateTime lets Windows PowerShell 5.1 serialize it as "/Date(ms)/", which
/// no ISO/locale parse accepts -- every restore point would then be dropped while the query still
/// reported success.
QString restorePointQueryScript() {
    return QStringLiteral(
        "$ErrorActionPreference='Stop'; try { "
        "Get-ComputerRestorePoint | "
        "Select-Object "
        "@{N='Date';E={$_.ConvertToDateTime($_.CreationTime).ToString('o')}}, "
        "Description | "
        "Sort-Object Date -Descending | "
        "ConvertTo-Json -Compress "
        "} catch { exit 1 }");
}

/// @brief The payload's restore-point records, or nullopt when it is not a record list at all.
///
/// Validate the shape instead of coercing it: a scalar payload is not a restore-point list, so
/// refuse it rather than read a bare number as a checkpoint. ConvertTo-Json emits a lone restore
/// point as an object rather than a one-element array, which IS a record list of one.
std::optional<QJsonArray> restorePointRecords(const QByteArray& output) {
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError) {
        return std::nullopt;
    }
    if (doc.isArray()) {
        return doc.array();
    }
    if (doc.isObject()) {
        QJsonArray single;
        single.append(doc.object());
        return single;
    }
    return std::nullopt;
}

/// @brief One record's (date, description), or nullopt when the record is not a restore point.
///
/// A record that is not an object, or whose Date is missing/wrong-typed/unparseable, means we
/// cannot enumerate the machine's checkpoints -- the caller reports a FAILED query rather than
/// silently skipping records and presenting the survivors as the complete list.
std::optional<QPair<QDateTime, QString>> restorePointFromRecord(const QJsonValue& record) {
    if (!record.isObject()) {
        return std::nullopt;
    }
    const QJsonObject obj = record.toObject();
    const QJsonValue date_value = obj.value(QStringLiteral("Date"));
    const QJsonValue description_value = obj.value(QStringLiteral("Description"));
    if (!date_value.isString()) {
        return std::nullopt;
    }
    if (!description_value.isString() && !description_value.isNull() &&
        !description_value.isUndefined()) {
        return std::nullopt;
    }
    const QDateTime dt = parseRestorePointDate(date_value.toString());
    if (!dt.isValid()) {
        return std::nullopt;
    }
    return QPair<QDateTime, QString>{dt, description_value.toString()};
}

}  // namespace

QVector<QPair<QDateTime, QString>> RestorePointManager::listRestorePoints() const {
    bool ignored = false;
    return listRestorePoints(ignored);
}

QVector<QPair<QDateTime, QString>> RestorePointManager::listRestorePoints(bool& queryOk) const {
    queryOk = false;
    QVector<QPair<QDateTime, QString>> points;

    // System32-qualified interpreter only; an unresolvable path is a FAILED query
    // (queryOk stays false), never a PATH-resolved probe.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        return points;
    }

    const auto result = sak::runProcess(powershell,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NonInteractive"),
                                         QStringLiteral("-Command"),
                                         restorePointQueryScript()},
                                        kCheckTimeoutMs);

    if (!result.succeeded()) {
        return points;  // query FAILED (non-zero exit / timeout) -> queryOk stays false
    }

    // A successful run with no restore points emits empty (or "null") output: that is a genuine
    // zero, not a failure -> queryOk true, empty list. Only a non-empty-but-unparseable payload
    // is treated as a failed query.
    const QByteArray output = result.std_out.trimmed().toUtf8();
    if (output.isEmpty() || output == "null") {
        queryOk = true;
        return points;
    }

    // Malformed, or parsed but not a record list -> a FAILED query (queryOk stays false), never
    // an empty result the caller would read as "this machine has no restore points".
    const std::optional<QJsonArray> records = restorePointRecords(output);
    if (!records) {
        return points;
    }

    // One unusable record fails the WHOLE enumeration: a partial list presented as complete is
    // what lets a caller believe a checkpoint it never saw does not exist.
    for (const auto& record : *records) {
        const std::optional<QPair<QDateTime, QString>> point = restorePointFromRecord(record);
        if (!point) {
            return {};
        }
        points.append(*point);
    }

    queryOk = true;
    return points;
}

bool RestorePointManager::isElevated() {
    return ElevationManager::isElevated();
}

}  // namespace sak
