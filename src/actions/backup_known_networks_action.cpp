// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file backup_known_networks_action.cpp
/// @brief Implements backup of saved Wi-Fi network profiles

#include "sak/actions/backup_known_networks_action.h"

#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace sak {

BackupKnownNetworksAction::BackupKnownNetworksAction(const QString& backup_location,
                                                     QObject* parent)
    : QuickAction(parent), m_backup_location(backup_location) {}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------

QList<BackupKnownNetworksAction::NetworkEntry> BackupKnownNetworksAction::collectProfiles() const {
    QList<NetworkEntry> result;

#ifndef Q_OS_WIN
    return result;
#else
    // Step 1: list all known profile names
    const ProcessResult listResult =
        runProcess("netsh", QStringList{"wlan", "show", "profiles"}, 10'000, [this]() {
            return isCancelled();
        });

    if (!listResult.succeeded()) {
        return result;
    }

    QStringList profileNames;
    const QRegularExpression nameRe(R"(:\s+(.+)$)");
    for (const QString& line : listResult.std_out.split('\n')) {
        const QString trimmed = line.trimmed();
        if (!trimmed.contains("All User Profile", Qt::CaseInsensitive) &&
            !trimmed.contains("Current User Profile", Qt::CaseInsensitive)) {
            continue;
        }

        const auto match = nameRe.match(trimmed);
        if (!match.hasMatch()) {
            continue;
        }

        const QString profile_name = match.captured(1).trimmed();
        if (!profile_name.isEmpty()) {
            profileNames.append(profile_name);
        }
    }

    // Step 2: fetch details for each profile
    const QRegularExpression keyRe(R"(Key Content\s*:\s+(.+))",
                                   QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression authRe(R"(Authentication\s*:\s+(.+))",
                                    QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression nbRe(R"(Network broadcast\s*:\s+(.+))",
                                  QRegularExpression::CaseInsensitiveOption);

    for (const QString& profile_name : profileNames) {
        if (isCancelled()) {
            break;
        }
        NetworkEntry entry = fetchProfileDetail(profile_name, keyRe, authRe, nbRe);
        if (!entry.ssid.isEmpty()) {
            result.append(entry);
        }
    }

    return result;
#endif
}

BackupKnownNetworksAction::NetworkEntry BackupKnownNetworksAction::fetchProfileDetail(
    const QString& name,
    const QRegularExpression& keyRe,
    const QRegularExpression& authRe,
    const QRegularExpression& nbRe) const {
    const ProcessResult detail =
        runProcess("netsh",
                   QStringList{"wlan", "show", "profile", "name=" + name, "key=clear"},
                   8000,
                   [this]() { return isCancelled(); });

    if (detail.timed_out) {
        return {};
    }

    NetworkEntry entry;
    entry.ssid = name;

    const auto keyMatch = keyRe.match(detail.std_out);
    if (keyMatch.hasMatch()) {
        entry.password = keyMatch.captured(1).trimmed();
    }

    entry.security = "WPA/WPA2/WPA3";
    const auto authMatch = authRe.match(detail.std_out);
    if (authMatch.hasMatch()) {
        const QString auth = authMatch.captured(1).trimmed().toUpper();
        if (auth.contains("WEP")) {
            entry.security = "WEP";
        } else if (auth == "OPEN" || auth.contains("NONE")) {
            entry.security = "None (Open)";
        }
    }

    const auto nbMatch = nbRe.match(detail.std_out);
    if (nbMatch.hasMatch()) {
        const QString nbVal = nbMatch.captured(1).trimmed();
        entry.hidden = nbVal.compare("Don't broadcast", Qt::CaseInsensitive) == 0 ||
                       nbVal.compare("Not broadcasting", Qt::CaseInsensitive) == 0;
    }

    return entry;
}

// -----------------------------------------------------------------------------
// scan
// -----------------------------------------------------------------------------

void BackupKnownNetworksAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);
    Q_EMIT scanProgress("Scanning for known WiFi profiles...");

    ScanResult result;

#ifndef Q_OS_WIN
    result.applicable = false;
    result.summary = "Not supported on this platform";
    Q_ASSERT(!result.summary.isEmpty());
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
    return;
#else
    const ProcessResult proc =
        runProcess("netsh", QStringList{"wlan", "show", "profiles"}, 10'000, [this]() {
            return isCancelled();
        });

    if (proc.timed_out) {
        result.applicable = false;
        result.summary = "netsh timed out";
        setScanResult(result);
        setStatus(ActionStatus::Ready);
        Q_EMIT scanComplete(result);
        return;
    }

    // Count profiles
    int count = 0;
    const QRegularExpression nameRe(R"(:\s+(.+)$)");
    for (const QString& line : proc.std_out.split('\n')) {
        const QString trimmed = line.trimmed();
        if (!trimmed.contains("All User Profile", Qt::CaseInsensitive) &&
            !trimmed.contains("Current User Profile", Qt::CaseInsensitive)) {
            continue;
        }

        const auto match = nameRe.match(trimmed);
        if (match.hasMatch() && !match.captured(1).trimmed().isEmpty()) {
            ++count;
        }
    }
    result.applicable = count > 0;
    result.files_count = count;
    result.summary = count > 0 ? QString("Found %1 known WiFi profile(s) to back up").arg(count)
                               : "No known WiFi profiles found";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
#endif
}

// -----------------------------------------------------------------------------
// execute
// -----------------------------------------------------------------------------

void BackupKnownNetworksAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Backup cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    const QDateTime startTime = QDateTime::currentDateTime();

    Q_EMIT executionProgress("Collecting WiFi profiles...", 20);

#ifndef Q_OS_WIN
    ExecutionResult result;
    result.success = false;
    result.message = "Backup Known Networks is only supported on Windows";
    finishWithResult(result, ActionStatus::Failed);
    return;
#else
    const QList<NetworkEntry> entries = collectProfiles();

    if (isCancelled()) {
        emitCancelledResult("Backup cancelled", startTime);
        return;
    }

    if (entries.isEmpty()) {
        ExecutionResult result;
        result.success = false;
        result.message = "No known WiFi profiles found";
        result.duration_ms = startTime.msecsTo(QDateTime::currentDateTime());
        finishWithResult(result, ActionStatus::Failed);
        return;
    }

    buildAndWriteBackup(entries, startTime);
#endif
}

void BackupKnownNetworksAction::buildAndWriteBackup(const QList<NetworkEntry>& entries,
                                                    const QDateTime& startTime) {
    Q_EMIT executionProgress("Writing JSON file...", 80);

    // Build JSON array in WiFi Manager format
    QJsonArray arr;
    for (const NetworkEntry& e : entries) {
        QJsonObject obj;
        obj["location"] = QString{};  // no location known from netsh
        obj["ssid"] = e.ssid;
        obj["password"] = e.password;
        obj["security"] = e.security;
        obj["hidden"] = e.hidden;
        arr.append(obj);
    }

    // Save to backup location
    QDir dir(m_backup_location);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            sak::logWarning("Failed to create network backup directory: {}",
                            m_backup_location.toStdString());
        }
    }

    const QString timestamp = startTime.toString("yyyy-MM-dd_HH-mm-ss");
    const QString filename = QString("wifi_networks_backup_%1.json").arg(timestamp);
    const QString filepath = dir.filePath(filename);

    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        ExecutionResult result;
        result.success = false;
        result.message = QString("Could not write to: %1").arg(filepath);
        result.duration_ms = startTime.msecsTo(QDateTime::currentDateTime());
        finishWithResult(result, ActionStatus::Failed);
        return;
    }
    const QByteArray data = QJsonDocument(arr).toJson();
    if (file.write(data) != data.size()) {
        sak::logError("Incomplete write of WiFi network backup");
        ExecutionResult result;
        Q_ASSERT(!result.success);
        result.success = false;
        result.message = QString("Incomplete write to: %1").arg(filepath);
        result.duration_ms = startTime.msecsTo(QDateTime::currentDateTime());
        finishWithResult(result, ActionStatus::Failed);
        return;
    }

    Q_EMIT executionProgress("Complete", 100);

    ExecutionResult result;
    result.success = true;
    result.files_processed = entries.size();
    result.message = QString("Backed up %1 WiFi network(s)").arg(entries.size());
    result.output_path = filepath;
    result.log = QString(
                     "Saved to: %1\nLoad this file in the WiFi Manager panel to "
                     "restore.")
                     .arg(filepath);
    result.duration_ms = startTime.msecsTo(QDateTime::currentDateTime());
    finishWithResult(result, ActionStatus::Success);
}

}  // namespace sak
