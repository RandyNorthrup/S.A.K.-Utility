// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/check_bloatware_action.h"
#include <QProcess>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace sak {

CheckBloatwareAction::CheckBloatwareAction(QObject* parent)
    : QuickAction(parent)
{
}

QVector<CheckBloatwareAction::BloatwareItem> CheckBloatwareAction::scanForBloatware() {
    QVector<BloatwareItem> bloatware;
    
    // Common bloatware patterns
    QStringList bloat_patterns = {
        "CandyCrush", "FarmVille", "BubbleWitch", "MarchofEmpires",
        "Minecraft", "Solitaire", "Xbox", "Zune", "BingNews",
        "BingWeather", "BingSports", "GetHelp", "Getstarted",
        "Messaging", "Microsoft3DViewer", "MicrosoftOfficeHub",
        "MicrosoftSolitaireCollection", "MixedReality", "OneConnect",
        "People", "Print3D", "SkypeApp", "Wallet", "WindowsAlarms",
        "windowscommunicationsapps", "WindowsFeedbackHub", "WindowsMaps",
        "WindowsSoundRecorder", "YourPhone", "ActiproSoftware"
    };
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-Command" << "Get-AppxPackage | Select-Object Name, PackageFullName");
    proc.waitForFinished(10000);
    
    QString output = proc.readAllStandardOutput();
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        for (const QString& pattern : bloat_patterns) {
            if (line.contains(pattern, Qt::CaseInsensitive)) {
                BloatwareItem app;
                app.name = line.trimmed();
                app.type = "Pre-installed Store App";
                app.size = 0;
                app.removal_method = "PowerShell Remove-AppxPackage";
                app.is_safe_to_remove = true;
                
                bloatware.append(app);
                break;
            }
        }
    }
    
    return bloatware;
}

void CheckBloatwareAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to check for bloatware";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void CheckBloatwareAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    QString report;
    QString structured_output;
    int bloatware_count = 0;
    int safe_to_remove = 0;
    qint64 total_size = 0;
    
    Q_EMIT executionProgress("Scanning for bloatware apps...", 10);
    
    // Phase 1: Scan UWP apps with Get-AppxPackage
    report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    report += "║                      BLOATWARE ANALYSIS                              ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ Phase 1: UWP App Scan (Get-AppxPackage)                             ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    QProcess ps_scan;
    ps_scan.setProgram("powershell.exe");
    ps_scan.setArguments(QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command"
        << R"(
            Get-AppxPackage | Select-Object Name, PackageFullName, InstallLocation, @{N='SizeMB';E={
                if ($_.InstallLocation -and (Test-Path $_.InstallLocation)) {
                    [Math]::Round((Get-ChildItem $_.InstallLocation -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1MB, 2)
                } else { 0 }
            }}, IsBundle, Architecture, Version, PublisherId | ConvertTo-Json
        )");
    
    ps_scan.start();
    ps_scan.waitForFinished(30000);
    QString scan_output = QString::fromUtf8(ps_scan.readAllStandardOutput()).trimmed();
    
    Q_EMIT executionProgress("Analyzing detected apps...", 40);
    
    // Comprehensive bloatware patterns with safety ratings
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
    bloatware_patterns["windowscommunicationsapps"] = qMakePair("Mail & Calendar", false);  // Some need this
    
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
    
    // Parse JSON output
    QJsonDocument doc = QJsonDocument::fromJson(scan_output.toUtf8());
    QJsonArray apps = doc.isArray() ? doc.array() : QJsonArray();
    if (apps.isEmpty() && doc.isObject()) {
        apps.append(doc.object());  // Handle single object response
    }
    
    QVector<QPair<QString, QPair<QString, double>>> detected_bloatware;  // Name -> (Category, SizeMB)
    
    for (const QJsonValue& val : apps) {
        QJsonObject app = val.toObject();
        QString name = app["Name"].toString();
        QString package_full = app["PackageFullName"].toString();
        double size_mb = app["SizeMB"].toDouble();
        
        // Check against patterns
        for (auto it = bloatware_patterns.begin(); it != bloatware_patterns.end(); ++it) {
            if (name.contains(it.key(), Qt::CaseInsensitive)) {
                bloatware_count++;
                detected_bloatware.append(qMakePair(name, qMakePair(it.value().first, size_mb)));
                total_size += static_cast<qint64>(size_mb * 1024 * 1024);
                if (it.value().second) safe_to_remove++;
                break;
            }
        }
    }
    
    Q_EMIT executionProgress("Generating detailed report...", 70);
    
    // Phase 2: Report generation
    report += QString("║ Apps Scanned: %1").arg(apps.size()).leftJustified(73, ' ') + "║\n";
    report += QString("║ Bloatware Found: %1").arg(bloatware_count).leftJustified(73, ' ') + "║\n";
    report += QString("║ Safe to Remove: %1").arg(safe_to_remove).leftJustified(73, ' ') + "║\n";
    report += QString("║ Total Size: %1 MB").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2)).leftJustified(73, ' ') + "║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    if (bloatware_count > 0) {
        report += "║ DETECTED BLOATWARE                                                   ║\n";
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";
        
        int displayed = 0;
        for (const auto& item : detected_bloatware) {
            if (displayed >= 20) break;  // Limit display
            QString name = item.first.left(40);
            QString category = item.second.first;
            double size_mb = item.second.second;
            
            report += QString("║ • %1").arg(name).leftJustified(73, ' ') + "║\n";
            report += QString("║   Category: %1 | Size: %2 MB")
                         .arg(category).arg(QString::number(size_mb, 'f', 2)).leftJustified(73, ' ') + "║\n";
            displayed++;
        }
        
        if (detected_bloatware.size() > 20) {
            report += QString("║   ... and %1 more app(s)").arg(detected_bloatware.size() - 20).leftJustified(73, ' ') + "║\n";
        }
    } else {
        report += "║ ✓ No common bloatware detected                                       ║\n";
    }
    
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ REMOVAL INFORMATION                                                  ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ To remove bloatware apps, use PowerShell:                            ║\n";
    report += "║                                                                      ║\n";
    report += "║ Remove for current user:                                             ║\n";
    report += "║   Get-AppxPackage *AppName* | Remove-AppxPackage                     ║\n";
    report += "║                                                                      ║\n";
    report += "║ Remove for all users (requires admin):                               ║\n";
    report += "║   Get-AppxPackage *AppName* -AllUsers | Remove-AppxPackage -AllUsers ║\n";
    report += "║                                                                      ║\n";
    report += "║ Remove provisioning (prevents reinstall):                            ║\n";
    report += "║   Get-AppxProvisionedPackage -Online | Where {$_.DisplayName -match  ║\n";
    report += "║   \"AppName\"} | Remove-AppxProvisionedPackage -Online                 ║\n";
    report += "║                                                                      ║\n";
    report += "║ ⚠ CAUTION: Some apps may be needed by certain users                  ║\n";
    report += "║   Always verify before removing system applications                  ║\n";
    report += "╚══════════════════════════════════════════════════════════════════════╝\n";
    
    Q_EMIT executionProgress("Analysis complete", 100);
    
    // Structured output
    structured_output += QString("APPS_SCANNED:%1\n").arg(apps.size());
    structured_output += QString("BLOATWARE_FOUND:%1\n").arg(bloatware_count);
    structured_output += QString("SAFE_TO_REMOVE:%1\n").arg(safe_to_remove);
    structured_output += QString("TOTAL_SIZE_MB:%1\n").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));
    structured_output += QString("SPACE_RECLAIMABLE_MB:%1\n").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));
    
    for (int i = 0; i < qMin(detected_bloatware.size(), 20); ++i) {
        structured_output += QString("BLOATWARE_%1:%2|%3|%4MB\n")
            .arg(i + 1)
            .arg(detected_bloatware[i].first)
            .arg(detected_bloatware[i].second.first)
            .arg(QString::number(detected_bloatware[i].second.second, 'f', 2));
    }
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = apps.size();
    result.success = true;
    result.message = bloatware_count > 0 
        ? QString("Found %1 bloatware app(s) using %2 MB").arg(bloatware_count).arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2))
        : "No common bloatware detected";
    result.log = report + "\n" + structured_output;
    
    setStatus(ActionStatus::Success);
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak
