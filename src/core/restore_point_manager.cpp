// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file restore_point_manager.cpp
/// @brief System Restore point creation and availability checking

#include "sak/restore_point_manager.h"

#include "sak/elevation_manager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

RestorePointManager::RestorePointManager(QObject* parent) : QObject(parent) {}

bool RestorePointManager::isSystemRestoreEnabled() const {
    QProcess ps;
    ps.setProgram("powershell.exe");
    ps.setArguments({"-NoProfile",
                     "-NonInteractive",
                     "-Command",
                     "try { "
                     "  $status = Get-ComputerRestorePoint -ErrorAction Stop; "
                     "  Write-Output 'ENABLED'; "
                     "} catch { "
                     "  try { "
                     "    $vss = Get-Service -Name VSS -ErrorAction Stop; "
                     "    if ($vss.Status -eq 'Running' -or $vss.StartType -ne 'Disabled') { "
                     "      Write-Output 'ENABLED'; "
                     "    } else { "
                     "      Write-Output 'DISABLED'; "
                     "    } "
                     "  } catch { "
                     "    Write-Output 'DISABLED'; "
                     "  } "
                     "}"});
    ps.start();

    if (!ps.waitForStarted(kCheckTimeoutMs) || !ps.waitForFinished(kCheckTimeoutMs)) {
        return false;
    }

    QString output = QString::fromUtf8(ps.readAllStandardOutput()).trimmed();
    return output.contains("ENABLED");
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

    QProcess ps;
    ps.setProgram("powershell.exe");
    ps.setArguments({"-NoProfile",
                     "-NonInteractive",
                     "-Command",
                     QString("try { "
                             "  Checkpoint-Computer -Description '%1' "
                             "    -RestorePointType 'APPLICATION_UNINSTALL' "
                             "    -ErrorAction Stop; "
                             "  Write-Output 'SUCCESS'; "
                             "} catch { "
                             "  Write-Error $_.Exception.Message; "
                             "  exit 1; "
                             "}")
                         .arg(safe_desc)});
    ps.start();

    if (!ps.waitForStarted(kCreateTimeoutMs)) {
        Q_EMIT restorePointFailed("Failed to start PowerShell for restore point creation.");
        return false;
    }

    if (!ps.waitForFinished(kCreateTimeoutMs)) {
        Q_EMIT restorePointFailed("Timeout creating restore point (exceeded 2 minutes).");
        return false;
    }

    if (ps.exitCode() != 0) {
        QString err = QString::fromUtf8(ps.readAllStandardError()).trimmed();
        if (err.isEmpty()) {
            err = QString::fromUtf8(ps.readAllStandardOutput()).trimmed();
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
    QVector<QPair<QDateTime, QString>> points;

    QProcess ps;
    ps.setProgram("powershell.exe");
    ps.setArguments({"-NoProfile",
                     "-NonInteractive",
                     "-Command",
                     "Get-ComputerRestorePoint -ErrorAction SilentlyContinue | "
                     "Select-Object @{N='Date';E={$_.ConvertToDateTime($_.CreationTime)}}, "
                     "Description | "
                     "Sort-Object Date -Descending | "
                     "ConvertTo-Json -Compress"});
    ps.start();

    if (!ps.waitForStarted(kCheckTimeoutMs) || !ps.waitForFinished(kCheckTimeoutMs)) {
        return points;
    }

    if (ps.exitCode() != 0) {
        return points;
    }

    QByteArray output = ps.readAllStandardOutput();
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError) {
        return points;
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

    return points;
}

bool RestorePointManager::isElevated() {
    return ElevationManager::isElevated();
}

}  // namespace sak
