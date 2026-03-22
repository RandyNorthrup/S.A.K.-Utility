// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file check_bloatware_action.cpp
/// @brief Implements detection and reporting of pre-installed bloatware applications

#include "sak/actions/check_bloatware_action.h"

#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace sak {

namespace {

constexpr int kBoxWidth = 69;
inline QString boxMid69() {
    return QChar(0x2560) + QString(kBoxWidth, QChar(0x2550)) + QChar(0x2563) + "\n";
}

QMap<QString, QPair<QString, bool>> buildBloatwarePatterns() {
    QMap<QString, QPair<QString, bool>> bloatware_patterns;  // Pattern -> (Category, SafeToRemove)

    // Games & Entertainment (SAFE)
    bloatware_patterns["CandyCrush"] = qMakePair("Game", true);
    bloatware_patterns["FarmVille"] = qMakePair("Game", true);
    bloatware_patterns["BubbleWitch"] = qMakePair("Game", true);
    bloatware_patterns["MarchofEmpires"] = qMakePair("Game", true);
    bloatware_patterns["Minecraft"] = qMakePair("Game", true);
    bloatware_patterns["Solitaire"] = qMakePair("Game", true);
    bloatware_patterns["Xbox"] = qMakePair("Gaming Service", true);
    bloatware_patterns["Zune"] = qMakePair("Media (Legacy)", true);

    // News & Info (SAFE)
    bloatware_patterns["BingNews"] = qMakePair("News", true);
    bloatware_patterns["BingWeather"] = qMakePair("Weather", true);
    bloatware_patterns["BingSports"] = qMakePair("Sports", true);
    bloatware_patterns["BingFinance"] = qMakePair("Finance", true);

    // Communication (CAUTION - some users may want these)
    bloatware_patterns["SkypeApp"] = qMakePair("Communication", true);
    bloatware_patterns["YourPhone"] = qMakePair("Phone Link", true);
    bloatware_patterns["PhoneLink"] = qMakePair("Phone Link", true);
    bloatware_patterns["Messaging"] = qMakePair("Communication", true);
    bloatware_patterns["windowscommunicationsapps"] = qMakePair("Mail & Calendar",
                                                                false);  // Some need this

    // Productivity & Tools (CAUTION)
    bloatware_patterns["GetHelp"] = qMakePair("Help", true);
    bloatware_patterns["Getstarted"] = qMakePair("Help", true);
    bloatware_patterns["MicrosoftOfficeHub"] = qMakePair("Office Ads", true);
    bloatware_patterns["WindowsMaps"] = qMakePair("Maps", true);
    bloatware_patterns["WindowsAlarms"] = qMakePair("Clock", true);
    bloatware_patterns["WindowsSoundRecorder"] = qMakePair("Recorder", true);
    bloatware_patterns["WindowsFeedbackHub"] = qMakePair("Feedback", true);
    bloatware_patterns["Wallet"] = qMakePair("Wallet (Legacy)", true);

    // 3D & Mixed Reality (SAFE for most)
    bloatware_patterns["Microsoft3DViewer"] = qMakePair("3D Viewer", true);
    bloatware_patterns["Print3D"] = qMakePair("3D Print", true);
    bloatware_patterns["MixedReality"] = qMakePair("Mixed Reality", true);

    // People & Social (SAFE)
    bloatware_patterns["People"] = qMakePair("People/Contacts", true);
    bloatware_patterns["OneConnect"] = qMakePair("Mobile Plans", true);

    // Third-party bloat (SAFE)
    bloatware_patterns["ActiproSoftware"] = qMakePair("Third-party", true);
    bloatware_patterns["king.com"] = qMakePair("Third-party Game", true);
    bloatware_patterns["Facebook"] = qMakePair("Social Media", true);
    bloatware_patterns["Twitter"] = qMakePair("Social Media", true);
    bloatware_patterns["LinkedIn"] = qMakePair("Professional", true);
    bloatware_patterns["Netflix"] = qMakePair("Streaming", true);
    bloatware_patterns["Spotify"] = qMakePair("Music", true);
    bloatware_patterns["Disney"] = qMakePair("Streaming", true);

    return bloatware_patterns;
}

/// @brief Find the first matching bloatware pattern for the given app name/package.
QMap<QString, QPair<QString, bool>>::const_iterator findBloatwareMatch(
    const QString& name,
    const QString& package_full,
    const QMap<QString, QPair<QString, bool>>& patterns) {
    for (auto it = patterns.constBegin(); it != patterns.constEnd(); ++it) {
        if (name.contains(it.key(), Qt::CaseInsensitive) ||
            package_full.contains(it.key(), Qt::CaseInsensitive)) {
            return it;
        }
    }
    return patterns.constEnd();
}

}  // namespace

CheckBloatwareAction::CheckBloatwareAction(QObject* parent) : QuickAction(parent) {}

CheckBloatwareAction::BloatwareItem CheckBloatwareAction::buildBloatwareItem(
    const QString& name,
    const QString& package_full,
    const QString& source,
    double size_mb,
    bool is_safe) {
    BloatwareItem item;
    item.name = name.isEmpty() ? package_full : name;
    item.type = source == "Provisioned" ? "Provisioned App" : "Installed Store App";
    item.size = static_cast<qint64>(size_mb * sak::kBytesPerMB);
    item.removal_method = source == "Provisioned" ? "PowerShell Remove-AppxProvisionedPackage"
                                                  : "PowerShell Remove-AppxPackage";
    item.is_safe_to_remove = is_safe;
    return item;
}

QVector<CheckBloatwareAction::BloatwareItem> CheckBloatwareAction::scanForBloatware() {
    QVector<BloatwareItem> bloatware;

    const QMap<QString, QPair<QString, bool>> bloatware_patterns = buildBloatwarePatterns();

    ProcessResult proc = runPowerShell(
        R"(
            $installed = Get-AppxPackage -AllUsers | Select-Object Name, PackageFullName,
                InstallLocation, @{N='Source';E={'Installed'}}, @{N='SizeMB';E={
                if ($_.InstallLocation -and (Test-Path $_.InstallLocation)) {
                    [Math]::Round((Get-ChildItem $_.InstallLocation -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1MB, 2)
                } else { 0 }
            }}
            $provisioned = Get-AppxProvisionedPackage -Online | Select-Object @{N='Name';E={$_.DisplayName}}, @{N='PackageFullName';E={$_.PackageName}}, @{N='InstallLocation';E={''}}, @{N='Source';E={'Provisioned'}}, @{N='SizeMB';E={0}}
            $all = @($installed +
                $provisioned) | Where-Object { $_.PackageFullName } | Sort-Object PackageFullName -Unique
            $all | ConvertTo-Json
        )",
        30'000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Bloatware scan warning: " + proc.std_err.trimmed());
    }

    const QString output = proc.std_out.trimmed();
    QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8());
    QJsonArray apps = doc.isArray() ? doc.array() : QJsonArray();
    if (apps.isEmpty() && doc.isObject()) {
        apps.append(doc.object());
    }

    QSet<QString> seen;
    for (const QJsonValue& val : apps) {
        QJsonObject app = val.toObject();
        const QString name = app["Name"].toString();
        const QString package_full = app["PackageFullName"].toString();
        const QString dedupe_key = package_full.isEmpty() ? name : package_full;
        if (dedupe_key.isEmpty() || seen.contains(dedupe_key)) {
            continue;
        }
        seen.insert(dedupe_key);

        auto match = findBloatwareMatch(name, package_full, bloatware_patterns);
        if (match == bloatware_patterns.constEnd()) {
            continue;
        }

        bloatware.append(buildBloatwareItem(name,
                                            package_full,
                                            app["Source"].toString(),
                                            app["SizeMB"].toDouble(),
                                            match.value().second));
    }

    return bloatware;
}

void CheckBloatwareAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    QVector<BloatwareItem> items = scanForBloatware();

    ScanResult result;
    result.applicable = !items.isEmpty();
    result.files_count = items.size();
    result.summary = result.applicable ? QString("Potential bloatware apps: %1").arg(items.size())
                                       : "No common bloatware detected";
    result.details = "Full scan reports removable apps and sizes";

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void CheckBloatwareAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Bloatware check cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    QString scan_output;
    BloatwareScanResult result;
    executeScanApps(start_time, scan_output, result.report);

    executeMatchBloatware(scan_output, result);

    executeBuildReport(start_time, result);
}

void CheckBloatwareAction::executeScanApps(const QDateTime& start_time,
                                           QString& scan_output,
                                           QString& report) {
    Q_UNUSED(start_time)
    Q_EMIT executionProgress("Scanning for bloatware apps...", 10);

    // Phase 1: Scan UWP apps with Get-AppxPackage
    report += "+======================================================================+\n";
    report += "|                      BLOATWARE ANALYSIS                              |\n";
    report += "+======================================================================+\n";
    report += "| Phase 1: UWP App Scan (All Users + Provisioned)                     |\n";
    report += "+======================================================================+\n";

    ProcessResult ps_scan = runPowerShell(
        R"(
            $installed = Get-AppxPackage -AllUsers | Select-Object Name, PackageFullName,
                InstallLocation, @{N='SizeMB';E={
                if ($_.InstallLocation -and (Test-Path $_.InstallLocation)) {
                    [Math]::Round((Get-ChildItem $_.InstallLocation -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1MB, 2)
                } else { 0 }
            }}, IsBundle, Architecture, Version, PublisherId, @{N='Source';E={'Installed'}}
            $provisioned = Get-AppxProvisionedPackage -Online | Select-Object @{N='Name';E={$_.DisplayName}}, @{N='PackageFullName';E={$_.PackageName}}, @{N='InstallLocation';E={''}}, @{N='SizeMB';E={0}}, @{N='Source';E={'Provisioned'}}
            $all = @($installed +
                $provisioned) | Where-Object { $_.PackageFullName } | Sort-Object PackageFullName -Unique
            $all | ConvertTo-Json
        )",
        30'000);
    if (!ps_scan.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Bloatware detail scan warning: " + ps_scan.std_err.trimmed());
    }
    scan_output = ps_scan.std_out.trimmed();
}

void CheckBloatwareAction::executeMatchBloatware(const QString& scan_output,
                                                 BloatwareScanResult& result) {
    Q_EMIT executionProgress("Analyzing detected apps...", 40);

    const QMap<QString, QPair<QString, bool>> bloatware_patterns = buildBloatwarePatterns();

    QJsonDocument doc = QJsonDocument::fromJson(scan_output.toUtf8());
    QJsonArray apps = doc.isArray() ? doc.array() : QJsonArray();
    if (apps.isEmpty() && doc.isObject()) {
        apps.append(doc.object());
    }
    result.apps_scanned = apps.size();

    ClassificationState state;
    for (const QJsonValue& val : apps) {
        classifyAppEntry(val.toObject(), bloatware_patterns, state, result);
    }

    BloatwareMatchStats stats{static_cast<int>(apps.size()),
                              state.installed_scanned,
                              state.provisioned_scanned,
                              state.safe_to_remove};
    formatBloatwareMatchReport(state.detected_bloatware, stats, result);
}

void CheckBloatwareAction::classifyAppEntry(
    const QJsonObject& app,
    const QMap<QString, QPair<QString, bool>>& bloatware_patterns,
    ClassificationState& state,
    BloatwareScanResult& result) {
    QString name = app["Name"].toString();
    QString package_full = app["PackageFullName"].toString();
    QString source = app["Source"].toString();
    double size_mb = app["SizeMB"].toDouble();

    const QString dedupe_key = package_full.isEmpty() ? name : package_full;
    if (dedupe_key.isEmpty() || state.seen.contains(dedupe_key)) {
        return;
    }
    state.seen.insert(dedupe_key);

    if (source == "Provisioned") {
        state.provisioned_scanned++;
    } else {
        state.installed_scanned++;
    }

    auto match = findBloatwareMatch(name, package_full, bloatware_patterns);
    if (match == bloatware_patterns.constEnd()) {
        return;
    }

    result.bloatware_count++;
    state.detected_bloatware.append(
        qMakePair(name.isEmpty() ? package_full : name, qMakePair(match.value().first, size_mb)));
    result.total_size += static_cast<qint64>(size_mb * sak::kBytesPerMB);
    if (match.value().second) {
        state.safe_to_remove++;
    }
}

void CheckBloatwareAction::formatBloatwareMatchReport(
    const QVector<QPair<QString, QPair<QString, double>>>& detected,
    const BloatwareMatchStats& stats,
    BloatwareScanResult& result) {
    Q_EMIT executionProgress("Generating detailed report...", 70);

    // Phase 2: Report generation
    result.report +=
        QString("\u2551 Apps Scanned: %1").arg(stats.apps_count).leftJustified(73, ' ') +
        "\u2551\n";
    result.report += QString("\u2551 Installed (All Users): %1")
                         .arg(stats.installed_scanned)
                         .leftJustified(73, ' ') +
                     "\u2551\n";
    result.report += QString("\u2551 Provisioned (System Image): %1")
                         .arg(stats.provisioned_scanned)
                         .leftJustified(73, ' ') +
                     "\u2551\n";
    result.report +=
        QString("\u2551 Bloatware Found: %1").arg(result.bloatware_count).leftJustified(73, ' ') +
        "\u2551\n";
    result.report +=
        QString("\u2551 Safe to Remove: %1").arg(stats.safe_to_remove).leftJustified(73, ' ') +
        "\u2551\n";
    qint64 total_size = 0;
    for (const auto& item : detected) {
        total_size += static_cast<qint64>(item.second.second * sak::kBytesPerMB);
    }
    result.report += QString("\u2551 Total Size: %1 MB")
                         .arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2))
                         .leftJustified(73, ' ') +
                     "\u2551\n";
    result.report += boxMid69();

    if (result.bloatware_count > 0) {
        result.report +=
            "\u2551 DETECTED BLOATWARE                                                   "
            "\u2551\n";
        result.report += boxMid69();

        const int display_limit = qMin(detected.size(), 20);
        for (int i = 0; i < display_limit; ++i) {
            const auto& item = detected[i];
            QString name = item.first.left(40);
            QString category = item.second.first;
            double size_mb = item.second.second;

            result.report += QString("\u2551 \u2022 %1").arg(name).leftJustified(73, ' ') +
                             "\u2551\n";
            result.report += QString("\u2551   Category: %1 | Size: %2 MB")
                                 .arg(category)
                                 .arg(QString::number(size_mb, 'f', 2))
                                 .leftJustified(73, ' ') +
                             "\u2551\n";
        }

        if (detected.size() > 20) {
            result.report += QString("\u2551   ... and %1 more app(s)")
                                 .arg(detected.size() - 20)
                                 .leftJustified(73, ' ') +
                             "\u2551\n";
        }
    } else {
        result.report +=
            "\u2551 \u2713 No common bloatware detected                                      "
            " \u2551\n";
    }

    // Structured output
    result.structured_output += QString("APPS_SCANNED:%1\n").arg(stats.apps_count);
    result.structured_output += QString("BLOATWARE_FOUND:%1\n").arg(result.bloatware_count);
    result.structured_output += QString("SAFE_TO_REMOVE:%1\n").arg(stats.safe_to_remove);
    result.structured_output += QString("INSTALLED_SCANNED:%1\n").arg(stats.installed_scanned);
    result.structured_output += QString("PROVISIONED_SCANNED:%1\n").arg(stats.provisioned_scanned);
    result.structured_output +=
        QString("TOTAL_SIZE_MB:%1\n").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));
    result.structured_output += QString("SPACE_RECLAIMABLE_MB:%1\n")
                                    .arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));

    for (int i = 0; i < qMin(detected.size(), 20); ++i) {
        result.structured_output += QString("BLOATWARE_%1:%2|%3|%4MB\n")
                                        .arg(i + 1)
                                        .arg(detected[i].first)
                                        .arg(detected[i].second.first)
                                        .arg(QString::number(detected[i].second.second, 'f', 2));
    }
}

void CheckBloatwareAction::executeBuildReport(const QDateTime& start_time,
                                              BloatwareScanResult& result) {
    result.report += "+======================================================================+\n";
    result.report += "| REMOVAL INFORMATION                                                  |\n";
    result.report += "+======================================================================+\n";
    result.report += "| To remove bloatware apps, use PowerShell:                            |\n";
    result.report += "|                                                                      |\n";
    result.report += "| Remove for current user:                                             |\n";
    result.report += "|   Get-AppxPackage *AppName* | Remove-AppxPackage                     |\n";
    result.report += "|                                                                      |\n";
    result.report += "| Remove for all users (requires admin):                               |\n";
    result.report += "|   Get-AppxPackage *AppName* -AllUsers | Remove-AppxPackage -AllUsers |\n";
    result.report += "|                                                                      |\n";
    result.report += "| Remove provisioning (prevents reinstall):                            |\n";
    result.report += "|   Get-AppxProvisionedPackage -Online | Where {$_.DisplayName -match  |\n";
    result.report += "|   \"AppName\"} | Remove-AppxProvisionedPackage -Online                 |\n";
    result.report += "|                                                                      |\n";
    result.report += "| (!) CAUTION: Some apps may be needed by certain users                  |\n";
    result.report += "|   Always verify before removing system applications                  |\n";
    result.report += "+======================================================================+\n";

    Q_EMIT executionProgress("Analysis complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult exec_result;
    exec_result.duration_ms = duration_ms;
    exec_result.files_processed = result.apps_scanned;
    exec_result.success = true;
    exec_result.message =
        result.bloatware_count > 0
            ? QString("Found %1 bloatware app(s) using %2 MB")
                  .arg(result.bloatware_count)
                  .arg(QString::number(result.total_size / (1024.0 * 1024.0), 'f', 2))
            : "No common bloatware detected";
    exec_result.log = result.report + "\n" + result.structured_output;
    finishWithResult(exec_result, ActionStatus::Success);
}

}  // namespace sak
