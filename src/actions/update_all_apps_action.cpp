// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/update_all_apps_action.h"
#include "sak/chocolatey_manager.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace sak {

UpdateAllAppsAction::UpdateAllAppsAction(QObject* parent)
    : QuickAction(parent)
    , m_choco_manager(std::make_unique<ChocolateyManager>())
{
}

void UpdateAllAppsAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to update applications";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void UpdateAllAppsAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    QString report;
    QString structured_output;
    int winget_available = 0;
    int winget_updated = 0;
    int store_updated = 0;
    int choco_updated = 0;
    int total_updated = 0;
    
    report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    report += "║                     APPLICATION UPDATE MANAGER                       ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    Q_EMIT executionProgress("Checking for winget availability...", 5);
    
    // Phase 1: Check winget availability
    bool winget_installed = (system("where winget > nul 2>&1") == 0);
    
    if (winget_installed) {
        report += "║ Phase 1: WinGet Package Updates                                     ║\n";
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";
        
        Q_EMIT executionProgress("Listing winget upgrades available...", 15);
        
        // Get list of upgradeable packages
        QProcess ps_list;
        ps_list.setProgram("powershell.exe");
        ps_list.setArguments(QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command"
            << "winget list --upgrade-available --accept-source-agreements | Select-String -Pattern '^' | Measure-Object -Line | Select-Object -ExpandProperty Lines");
        ps_list.start();
        ps_list.waitForFinished(20000);
        QString count_output = QString::fromUtf8(ps_list.readAllStandardOutput()).trimmed();
        winget_available = count_output.toInt();
        
        if (winget_available > 3) {  // Header lines are ~3
            winget_available -= 3;
            report += QString("║ Upgrades Available: %1").arg(winget_available).leftJustified(73, ' ') + "║\n";
            
            Q_EMIT executionProgress("Upgrading winget packages...", 30);
            
            // Upgrade all packages
            QProcess ps_upgrade;
            ps_upgrade.setProgram("powershell.exe");
            ps_upgrade.setArguments(QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command"
                << "winget upgrade --all --include-unknown --silent --accept-package-agreements --accept-source-agreements 2>&1 | Out-String");
            ps_upgrade.start();
            ps_upgrade.waitForFinished(300000);  // 5 minutes max
            QString upgrade_output = QString::fromUtf8(ps_upgrade.readAllStandardOutput()).trimmed();
            
            // Count successful upgrades
            winget_updated = upgrade_output.count("Successfully installed", Qt::CaseInsensitive);
            total_updated += winget_updated;
            
            report += QString("║ Successfully Updated: %1").arg(winget_updated).leftJustified(73, ' ') + "║\n";
        } else {
            report += "║ No WinGet upgrades available                                         ║\n";
        }
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    } else {
        report += "║ Phase 1: WinGet - Not Available                                     ║\n";
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    }
    
    Q_EMIT executionProgress("Checking Microsoft Store updates...", 50);
    
    // Phase 2: Microsoft Store updates
    report += "║ Phase 2: Microsoft Store App Updates                                ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    QProcess ps_store;
    ps_store.setProgram("powershell.exe");
    ps_store.setArguments(QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command"
        << "try { $namespaceName = 'root\\cimv2\\mdm\\dmmap'; $className = 'MDM_EnterpriseModernAppManagement_AppManagement01'; $wmiObj = Get-CimInstance -Namespace $namespaceName -ClassName $className; $result = $wmiObj | Invoke-CimMethod -MethodName UpdateScanMethod; Write-Output 'STORE_UPDATE_TRIGGERED:YES' } catch { Write-Output 'STORE_UPDATE_TRIGGERED:NO'; Write-Output \"ERROR:$($_.Exception.Message)\" }");
    ps_store.start();
    ps_store.waitForFinished(30000);
    QString store_output = QString::fromUtf8(ps_store.readAllStandardOutput()).trimmed();
    
    if (store_output.contains("STORE_UPDATE_TRIGGERED:YES")) {
        report += "║ Store update check triggered successfully                           ║\n";
        report += "║ Note: Store apps update automatically in background                 ║\n";
        store_updated = 1;  // Flag that trigger succeeded
    } else {
        report += "║ Store update trigger not available (requires admin or policy)       ║\n";
    }
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    Q_EMIT executionProgress("Checking Chocolatey updates...", 70);
    
    // Phase 3: Chocolatey (if available)
    bool choco_installed = (system("where choco > nul 2>&1") == 0);
    
    if (choco_installed) {
        report += "║ Phase 3: Chocolatey Package Updates                                 ║\n";
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";
        
        QStringList outdated_packages = m_choco_manager->getOutdatedPackages();
        
        if (!outdated_packages.isEmpty()) {
            report += QString("║ Outdated Packages: %1").arg(outdated_packages.size()).leftJustified(73, ' ') + "║\n";
            
            for (const QString& package : outdated_packages) {
                if (isCancelled()) break;
                
                ChocolateyManager::InstallConfig config;
                config.package_name = package;
                config.force = true;
                
                if (m_choco_manager->installPackage(config).success) {
                    choco_updated++;
                    total_updated++;
                }
            }
            
            report += QString("║ Successfully Updated: %1").arg(choco_updated).leftJustified(73, ' ') + "║\n";
        } else {
            report += "║ No Chocolatey updates available                                     ║\n";
        }
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    } else {
        report += "║ Phase 3: Chocolatey - Not Available                                 ║\n";
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    }
    
    Q_EMIT executionProgress("Update complete", 100);
    
    // Summary
    report += "║ UPDATE SUMMARY                                                       ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ WinGet: %1 available, %2 updated").arg(winget_available).arg(winget_updated).leftJustified(73, ' ') + "║\n";
    report += QString("║ Microsoft Store: %1").arg(store_updated > 0 ? "Update triggered" : "Not triggered").leftJustified(73, ' ') + "║\n";
    report += QString("║ Chocolatey: %1 updated").arg(choco_updated).leftJustified(73, ' ') + "║\n";
    report += QString("║ Total Updates: %1").arg(total_updated).leftJustified(73, ' ') + "║\n";
    report += "║                                                                      ║\n";
    
    if (total_updated > 0 || store_updated > 0) {
        report += "║ ✓ Application updates completed                                      ║\n";
        if (total_updated > 0) {
            report += "║   Some applications may require restart                              ║\n";
        }
    } else if (winget_installed || choco_installed) {
        report += "║ ✓ All applications are up to date                                    ║\n";
    } else {
        report += "║ ⚠ No package managers available                                      ║\n";
        report += "║   Install WinGet or Chocolatey for app update management            ║\n";
    }
    
    report += "║                                                                      ║\n";
    report += "║ PACKAGE MANAGERS:                                                    ║\n";
    report += QString("║  • WinGet: %1").arg(winget_installed ? "✓ Installed" : "✗ Not installed").leftJustified(73, ' ') + "║\n";
    report += QString("║  • Microsoft Store: %1").arg(store_updated > 0 ? "✓ Available" : "- Limited access").leftJustified(73, ' ') + "║\n";
    report += QString("║  • Chocolatey: %1").arg(choco_installed ? "✓ Installed" : "✗ Not installed").leftJustified(73, ' ') + "║\n";
    report += "╚══════════════════════════════════════════════════════════════════════╝\n";
    
    // Structured output
    structured_output += QString("WINGET_INSTALLED:%1\n").arg(winget_installed ? "YES" : "NO");
    structured_output += QString("WINGET_AVAILABLE:%1\n").arg(winget_available);
    structured_output += QString("WINGET_UPDATED:%1\n").arg(winget_updated);
    structured_output += QString("STORE_TRIGGERED:%1\n").arg(store_updated > 0 ? "YES" : "NO");
    structured_output += QString("CHOCO_INSTALLED:%1\n").arg(choco_installed ? "YES" : "NO");
    structured_output += QString("CHOCO_UPDATED:%1\n").arg(choco_updated);
    structured_output += QString("TOTAL_UPDATED:%1\n").arg(total_updated);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = total_updated;
    result.success = true;
    result.message = total_updated > 0 || store_updated > 0
        ? QString("Updated %1 package(s)").arg(total_updated) + (store_updated > 0 ? " + Store triggered" : "")
        : (winget_installed || choco_installed) ? "All applications up to date" : "No package managers available";
    result.log = report + "\n" + structured_output;
    
    setStatus(ActionStatus::Success);
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak
