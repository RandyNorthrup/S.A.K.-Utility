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
    const auto result = sak::runProcess(
        QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"),
         QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"),
         QStringLiteral(
             "try { "
             "  $k = Get-ItemProperty "
             "    -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore' "
             "    -Name 'RPSessionInterval' -ErrorAction Stop; "
             "  if ([int]$k.RPSessionInterval -gt 0) { Write-Output 'ENABLED'; } "
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

bool RestorePointManager::createRestorePoint(const QString& description) {
    Q_ASSERT(!description.isEmpty());
    if (!isElevated()) {
        Q_EMIT restorePointFailed("Creating restore points requires administrator privileges.");
        return false;
    }

    // Truncate description to max length
    QString safe_desc = description.left(kMaxDescriptionLength);

    // Escape single quotes in description
    safe_desc.replace("'", "''");

    const auto result = sak::runProcess(QStringLiteral("powershell.exe"),
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NonInteractive"),
                                         QStringLiteral("-Command"),
                                         QString("try { "
                                                 "  Checkpoint-Computer -Description '%1' "
                                                 "    -RestorePointType 'APPLICATION_UNINSTALL' "
                                                 "    -ErrorAction Stop "
                                                 "    -WarningVariable wv "
                                                 "    -WarningAction SilentlyContinue; "
                                                 // Checkpoint-Computer does NOT throw when Windows
                                                 // SKIPS creation under the once-per-24h frequency
                                                 // throttle -- it only writes a warning (and exits
                                                 // 0). Fail on that warning so a silent no-op is
                                                 // never reported as a created restore point; its
                                                 // '1440'/'frequency' text routes to the throttle
                                                 // branch below.
                                                 "  if ($wv) { Write-Error $wv; exit 1; } "
                                                 "  Write-Output 'SUCCESS'; "
                                                 "} catch { "
                                                 "  Write-Error $_.Exception.Message; "
                                                 "  exit 1; "
                                                 "}")
                                             .arg(safe_desc)},
                                        kCreateTimeoutMs);

    if (result.timed_out) {
        Q_EMIT restorePointFailed("Timeout creating restore point (exceeded 2 minutes).");
        return false;
    }

    if (result.exit_code != 0) {
        QString err = result.std_err.trimmed();
        if (err.isEmpty()) {
            err = result.std_out.trimmed();
        }

        // Check for throttle (Windows only allows one per 24h in some configs)
        if (err.contains("frequency", Qt::CaseInsensitive) ||
            err.contains("1440", Qt::CaseInsensitive)) {
            Q_EMIT restorePointFailed(
                "Windows limits restore point creation to once per 24 hours. "
                "A recent restore point already exists.");
        } else {
            Q_EMIT restorePointFailed(QString("Failed to create restore point: %1").arg(err));
        }
        return false;
    }

    Q_EMIT restorePointCreated(safe_desc);
    return true;
}

namespace {

QDateTime parseRestorePointDate(const QString& date_str) {
    QDateTime dt = QDateTime::fromString(date_str, Qt::ISODateWithMs);
    if (dt.isValid()) {
        return dt;
    }
    dt = QDateTime::fromString(date_str, Qt::ISODate);
    if (dt.isValid()) {
        return dt;
    }
    return QDateTime::fromString(date_str, "M/d/yyyy h:mm:ss AP");
}

}  // namespace

QVector<QPair<QDateTime, QString>> RestorePointManager::listRestorePoints() const {
    bool ignored = false;
    return listRestorePoints(ignored);
}

QVector<QPair<QDateTime, QString>> RestorePointManager::listRestorePoints(bool& queryOk) const {
    queryOk = false;
    QVector<QPair<QDateTime, QString>> points;

    const auto result = sak::runProcess(
        QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"),
         QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"),
         QStringLiteral("Get-ComputerRestorePoint -ErrorAction SilentlyContinue | "
                        "Select-Object @{N='Date';E={$_.ConvertToDateTime($_.CreationTime)}}, "
                        "Description | "
                        "Sort-Object Date -Descending | "
                        "ConvertTo-Json -Compress")},
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

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError) {
        return points;  // succeeded but malformed -> treat as a failed query (queryOk false)
    }

    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        arr.append(doc.object());
    }

    for (const auto& val : arr) {
        QJsonObject obj = val.toObject();
        QDateTime dt = parseRestorePointDate(obj["Date"].toString());
        if (dt.isValid()) {
            points.append({dt, obj["Description"].toString()});
        }
    }

    queryOk = true;
    return points;
}

bool RestorePointManager::isElevated() {
    return ElevationManager::isElevated();
}

}  // namespace sak
