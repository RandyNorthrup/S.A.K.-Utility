// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file update_all_apps_action.cpp
/// @brief Implements batch application updates via Chocolatey package manager

#include "sak/actions/update_all_apps_action.h"
#include "sak/chocolatey_manager.h"
#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/layout_constants.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>

namespace sak {

UpdateAllAppsAction::UpdateAllAppsAction(QObject* parent)
    : QuickAction(parent)
    , m_choco_manager(std::make_unique<ChocolateyManager>())
{
}

void UpdateAllAppsAction::scan() {
    setStatus(ActionStatus::Scanning);

    bool winget_installed = (system("where winget > nul 2>&1") == 0);
    bool choco_installed = (system("where choco > nul 2>&1") == 0);

    int winget_available = 0;
    int choco_available = 0;

    if (winget_installed) {
        ProcessResult ps_list = runPowerShell(
            "winget list --upgrade-available --accept-source-agreements | Select-String -Pattern "
            "'^' | Measure-Object -Line | Select-Object -ExpandProperty Lines",
            15000);
        if (!ps_list.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Winget list warning: " + ps_list.std_err.trimmed());
        }
        QString count_output = ps_list.std_out.trimmed();
        bool ok = false;
        winget_available = count_output.toInt(&ok);
        if (!ok) winget_available = 0;
        if (winget_available > 3) {
            winget_available -= 3; // Header lines
        } else {
            winget_available = 0;
        }
    }

    if (choco_installed) {
        QProcess choco_list;
        choco_list.setProgram("choco");
        choco_list.setArguments(QStringList() << "outdated" << "-r");
        choco_list.start();
        if (!choco_list.waitForFinished(sak::kTimeoutChocoListMs)) {
            sak::logWarning("Chocolatey outdated check timed out after 15s");
            choco_list.kill();
        } else {
            const QString output = QString::fromUtf8(choco_list.readAllStandardOutput());
            choco_available = output.split('\n', Qt::SkipEmptyParts).count();
        }
    }

    ScanResult result;
    result.applicable = winget_installed || choco_installed;
    result.files_count = winget_available + choco_available;
    result.estimated_duration_ms = 0;

    if (result.applicable) {
        result.summary = QString("Updates available: %1").arg(result.files_count);
        result.details = QString("WinGet: %1, Chocolatey: %2")
            .arg(winget_available)
            .arg(choco_available);
    } else {
        result.summary = "No package managers detected";
        result.details = "Install WinGet or Chocolatey to manage updates";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void UpdateAllAppsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("Application update cancelled"));
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    UpdateSummary summary;
    summary.report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    summary.report += "║                     APPLICATION UPDATE MANAGER                       ║\n";
    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    Q_EMIT executionProgress("Checking for winget availability...", 5);

    // Phase 1: WinGet
    if (!runWingetUpdate(summary, start_time)) return;

    // Phase 2: Microsoft Store
    Q_EMIT executionProgress("Checking Microsoft Store updates...", 50);
    if (isCancelled()) { emitCancelledResult("Application update cancelled", start_time); return; }
    if (!runStoreUpdate(summary)) return;

    // Phase 3: Chocolatey
    Q_EMIT executionProgress("Checking Chocolatey updates...", 70);
    if (isCancelled()) { emitCancelledResult("Application update cancelled", start_time); return; }
    if (!runChocoUpdate(summary, start_time)) return;

    Q_EMIT executionProgress("Update complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    buildUpdateReport(summary, duration_ms);

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = summary.total_updated;
    result.success = true;
    if (summary.total_updated > 0 || summary.store_updated > 0) {
        result.message = QString("Updated %1 package(s)").arg(summary.total_updated);
        if (summary.store_updated > 0) {
            result.message += " + Store triggered";
        }
    } else if (summary.winget_installed || summary.choco_installed) {
        result.message = "All applications up to date";
    } else {
        result.message = "No package managers available";
    }
    result.log = summary.report + "\n" + summary.structured_output;

    finishWithResult(result, ActionStatus::Success);
}

// ─── Private Helpers ────────────────────────────────────────────────────────────

bool UpdateAllAppsAction::runWingetUpdate(UpdateSummary& summary, const QDateTime& start_time) {
    summary.winget_installed = (system("where winget > nul 2>&1") == 0);

    if (!summary.winget_installed) {
        summary.report += "║ Phase 1: WinGet - Not Available                                     "
                          "║\n";
        summary
            .report += "╠══════════════════════════════════════════════════════════════════════╣\n";
        return true;
    }

    summary.report += "║ Phase 1: WinGet Package Updates                                     ║\n";
    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    Q_EMIT executionProgress("Listing winget upgrades available...", 15);

    ProcessResult ps_list = runPowerShell(
        "winget list --upgrade-available --accept-source-agreements | Select-String -Pattern '^' | "
        "Measure-Object -Line | Select-Object -ExpandProperty Lines",
        20000);
    if (!ps_list.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Winget list warning: " + ps_list.std_err.trimmed());
    }
    bool ok = false;
    summary.winget_available = ps_list.std_out.trimmed().toInt(&ok);
    if (!ok) summary.winget_available = 0;

    if (summary.winget_available <= 3) {
        summary.report += "║ No WinGet upgrades available                                         "
                          "║\n";
        summary
            .report += "╠══════════════════════════════════════════════════════════════════════╣\n";
        return true;
    }

    summary.winget_available -= 3;
    summary.report += QString("║ Upgrades Available: %1")
        .arg(summary.winget_available).leftJustified(73, ' ') + "║\n";

    Q_EMIT executionProgress("Upgrading winget packages...", 30);

    if (isCancelled()) {
        emitCancelledResult("Application update cancelled", start_time);
        return false;
    }

    ProcessResult ps_upgrade = runPowerShell(
        "winget upgrade --all --include-unknown --silent --accept-package-agreements "
        "--accept-source-agreements 2>&1 | Out-String",
        300000);
    if (!ps_upgrade.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Winget upgrade warning: " + ps_upgrade.std_err.trimmed());
    }

    summary.winget_updated = ps_upgrade.std_out.count("Successfully installed",
        Qt::CaseInsensitive);
    summary.total_updated += summary.winget_updated;

    summary.report += QString("║ Successfully Updated: %1")
        .arg(summary.winget_updated).leftJustified(73, ' ') + "║\n";

    QString upgrade_errors = ps_upgrade.std_err.trimmed();
    if (!upgrade_errors.isEmpty()) {
        summary.report += "║ Winget Errors:                                                  ║\n";
        const QStringList error_lines = upgrade_errors.split('\n', Qt::SkipEmptyParts);
        const int max_lines = std::min(5, static_cast<int>(error_lines.size()));
        for (int i = 0; i < max_lines; ++i) {
            summary.report += QString("║  • %1").arg(error_lines[i].left(67)).leftJustified(73,
                ' ') + "║\n";
        }
    }

    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    return true;
}

bool UpdateAllAppsAction::runStoreUpdate(UpdateSummary& summary) {
    summary.report += "║ Phase 2: Microsoft Store App Updates                                ║\n";
    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    ProcessResult ps_store = runPowerShell(
        "try { $namespaceName = 'root\\cimv2\\mdm\\dmmap'; $className = "
        "'MDM_EnterpriseModernAppManagement_AppManagement01'; $wmiObj = Get-CimInstance -Namespace "
        "$namespaceName -ClassName $className; $result = $wmiObj | Invoke-CimMethod -MethodName "
        "UpdateScanMethod; Write-Output 'STORE_UPDATE_TRIGGERED:YES' } catch { Write-Output "
        "'STORE_UPDATE_TRIGGERED:NO'; Write-Output \"ERROR:$($_.Exception.Message)\" }",
        30000);
    if (!ps_store.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Store update trigger warning: " + ps_store.std_err.trimmed());
    }

    if (ps_store.std_out.trimmed().contains("STORE_UPDATE_TRIGGERED:YES")) {
        summary.report += "║ Store update check triggered successfully                           "
                          "║\n";
        summary.report += "║ Note: Store apps update automatically in background                 "
                          "║\n";
        summary.store_updated = 1;
    } else {
        summary.report += "║ Store update trigger not available (requires admin or policy)       "
                          "║\n";
    }
    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    return true;
}

bool UpdateAllAppsAction::runChocoUpdate(UpdateSummary& summary, const QDateTime& start_time) {
    summary.choco_installed = (system("where choco > nul 2>&1") == 0);

    if (!summary.choco_installed) {
        summary.report += "║ Phase 3: Chocolatey - Not Available                                 "
                          "║\n";
        summary
            .report += "╠══════════════════════════════════════════════════════════════════════╣\n";
        return true;
    }

    summary.report += "║ Phase 3: Chocolatey Package Updates                                 ║\n";
    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    QStringList outdated_packages = m_choco_manager->getOutdatedPackages();

    if (outdated_packages.isEmpty()) {
        summary.report += "║ No Chocolatey updates available                                     "
                          "║\n";
        summary
            .report += "╠══════════════════════════════════════════════════════════════════════╣\n";
        return true;
    }

    summary.report += QString("║ Outdated Packages: %1")
        .arg(outdated_packages.size()).leftJustified(73, ' ') + "║\n";

    for (const QString& package : outdated_packages) {
        if (isCancelled()) break;

        ChocolateyManager::InstallConfig config;
        config.package_name = package;
        config.force = true;

        if (m_choco_manager->installPackage(config).success) {
            summary.choco_updated++;
            summary.total_updated++;
        }
    }

    if (isCancelled()) {
        emitCancelledResult("Application update cancelled", start_time);
        return false;
    }

    summary.report += QString("║ Successfully Updated: %1")
        .arg(summary.choco_updated).leftJustified(73, ' ') + "║\n";
    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    return true;
}

void UpdateAllAppsAction::buildUpdateReport(UpdateSummary& summary, qint64 duration_ms) const {
    Q_UNUSED(duration_ms)

    summary.report += "║ UPDATE SUMMARY                                                       ║\n";
    summary.report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    summary.report += QString("║ WinGet: %1 available, %2 updated").arg(summary.winget_available)
        .arg(summary.winget_updated).leftJustified(73, ' ') + "║\n";
    summary.report +=
        QString("║ Microsoft Store: %1")
            .arg(summary.store_updated > 0
                     ? "Update triggered"
                     : "Not triggered")
            .leftJustified(73, ' ')
        + "║\n";
    summary.report += QString("║ Chocolatey: %1 updated")
        .arg(summary.choco_updated).leftJustified(73, ' ') + "║\n";
    summary.report += QString("║ Total Updates: %1").arg(summary.total_updated).leftJustified(73,
        ' ') + "║\n";
    summary.report += "║                                                                      ║\n";

    if (summary.total_updated > 0 || summary.store_updated > 0) {
        summary.report += "║ ✓ Application updates completed                                      "
                          "║\n";
        if (summary.total_updated > 0) {
            summary.report += "║   Some applications may require restart                           "
                              "   ║\n";
        }
    } else if (summary.winget_installed || summary.choco_installed) {
        summary.report += "║ ✓ All applications are up to date                                    "
                          "║\n";
    } else {
        summary.report += "║ ⚠ No package managers available                                      "
                          "║\n";
        summary.report += "║   Install WinGet or Chocolatey for app update management            "
                          "║\n";
    }

    summary.report += "║                                                                      ║\n";
    summary.report += "║ PACKAGE MANAGERS:                                                    ║\n";
    summary.report += QString("║  • WinGet: %1")
        .arg(summary.winget_installed ? "✓ Installed" : "✗ Not installed")
            .leftJustified(73, ' ') + "║\n";
    summary.report += QString("║  • Microsoft Store: %1")
        .arg(summary.store_updated > 0 ? "✓ Available" : "- Limited access")
            .leftJustified(73, ' ') + "║\n";
    summary.report += QString("║  • Chocolatey: %1")
        .arg(summary.choco_installed ? "✓ Installed" : "✗ Not installed")
            .leftJustified(73, ' ') + "║\n";
    summary.report += "╚══════════════════════════════════════════════════════════════════════╝\n";

    summary.structured_output += QString("WINGET_INSTALLED:%1\n")
        .arg(summary.winget_installed ? "YES" : "NO");
    summary.structured_output += QString("WINGET_AVAILABLE:%1\n").arg(summary.winget_available);
    summary.structured_output += QString("WINGET_UPDATED:%1\n").arg(summary.winget_updated);
    summary.structured_output += QString("STORE_TRIGGERED:%1\n")
        .arg(summary.store_updated > 0 ? "YES" : "NO");
    summary.structured_output += QString("CHOCO_INSTALLED:%1\n")
        .arg(summary.choco_installed ? "YES" : "NO");
    summary.structured_output += QString("CHOCO_UPDATED:%1\n").arg(summary.choco_updated);
    summary.structured_output += QString("TOTAL_UPDATED:%1\n").arg(summary.total_updated);
}

} // namespace sak
