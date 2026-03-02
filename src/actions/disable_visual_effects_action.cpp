// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file disable_visual_effects_action.cpp
/// @brief Implements Windows visual effects optimization for performance

#include "sak/actions/disable_visual_effects_action.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"
#include <QSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace sak {

DisableVisualEffectsAction::DisableVisualEffectsAction(QObject* parent)
    : QuickAction(parent)
{
}

bool DisableVisualEffectsAction::areVisualEffectsEnabled() {
    QSettings settings(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects",
                      QSettings::NativeFormat);

    int value = settings.value("VisualFXSetting", 2).toInt();
    // 0 = Let Windows choose, 1 = Best appearance, 2 = Best performance, 3 = Custom
    return (value != 2);
}

bool DisableVisualEffectsAction::disableVisualEffects() {
    // Set to "Best Performance"
    QSettings settings(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects",
                      QSettings::NativeFormat);
    settings.setValue("VisualFXSetting", 2);

    // Disable specific effects
    QSettings dwm("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\DWM",
                 QSettings::NativeFormat);
    dwm.setValue("EnableAeroPeek", 0);
    dwm.setValue("AlwaysHibernateThumbnails", 0);

    QSettings advanced("HKEY_CURRENT_USER\\Control Panel\\Desktop\\WindowMetrics",
                      QSettings::NativeFormat);
    advanced.setValue("MinAnimate", "0");

    // Notify system of changes
    ProcessResult proc = runProcess("rundll32.exe",
        QStringList() << "user32.dll,UpdatePerUserSystemParameters" << "1" << "True",
            sak::kTimeoutProcessShortMs);
    if (!proc.succeeded()) {
        Q_EMIT logMessage("Visual effects notify warning: " + proc.std_err.trimmed());
    }

    return true;
}

void DisableVisualEffectsAction::scan() {
    setStatus(ActionStatus::Scanning);

    bool enabled = areVisualEffectsEnabled();

    ScanResult result;
    result.applicable = enabled;
    result.summary = enabled
        ? "Visual effects are enabled"
        : "Visual effects already optimized for performance";
    result.details = "Optimization sets Best Performance and disables animations";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void DisableVisualEffectsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Visual effects change cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    Q_EMIT executionProgress("Analyzing current visual effects settings...", 10);

    // Phase 1: Enumerate current settings
    QString report;
    report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    report += "║                   VISUAL EFFECTS OPTIMIZATION                        ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ Phase 1: Current Settings Analysis                                  ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    ProcessResult ps_check = runPowerShell(buildCheckSettingsScript(),
        sak::kTimeoutProcessMediumMs);
    if (!ps_check.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Visual effects check warning: " + ps_check.std_err.trimmed());
    }

    QJsonDocument check_doc = QJsonDocument::fromJson(ps_check.std_out.trimmed().toUtf8());
    QJsonObject current_settings = check_doc.object();

    int visual_fx = current_settings["VisualFXSetting"].toInt(-1);
    QString fx_mode = (visual_fx == 0) ? "Let Windows choose" :
                      (visual_fx == 1) ? "Best appearance" :
                      (visual_fx == 2) ? "Best performance" :
                      (visual_fx == 3) ? "Custom" : "Unknown";

    report += buildCurrentSettingsReport(current_settings);

    Q_EMIT executionProgress("Applying Best Performance settings...", 35);

    // Phase 2: Apply settings
    int settings_changed = 0;
    int settings_total = 0;
    applyVisualEffectsSettings(report, settings_changed, settings_total);

    Q_EMIT executionProgress("Notifying system of changes...", 70);

    // Phase 3: Notify system
    report += "║ Phase 3: System Notification                                        ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    ProcessResult notify_proc = runProcess("rundll32.exe",
        QStringList() << "user32.dll,UpdatePerUserSystemParameters" << "1" << "True", 5000);
    if (!notify_proc.succeeded()) {
        Q_EMIT logMessage("Visual effects notify warning: " + notify_proc.std_err.trimmed());
    }
    bool notification_success = notify_proc.succeeded();

    report += QString("║ System Notification: %1")
        .arg(notification_success ? "✓ Success" : "✗ Failed").leftJustified(73, ' ') + "║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    Q_EMIT executionProgress("Optimization complete", 100);

    // Phase 4: Summary
    report += buildSummaryReport(settings_total, settings_changed, fx_mode);

    buildAndFinishVisualEffectsResult(report, settings_total, settings_changed,
                                      notification_success, fx_mode, start_time);
}

void DisableVisualEffectsAction::applyVisualEffectsSettings(
    QString& report, int& settings_changed, int& settings_total)
{
    report += "║ Phase 2: Applying Best Performance Settings                         ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    ProcessResult ps_apply = runPowerShell(buildApplySettingsScript(),
        sak::kTimeoutProcessMediumMs);
    if (!ps_apply.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Visual effects apply warning: " + ps_apply.std_err.trimmed());
    }

    settings_changed = 0;
    settings_total = 0;
    for (const QString& line : ps_apply.std_out.trimmed().split('\n')) {
        if (line.startsWith("CHANGES:")) {
            settings_changed = line.mid(8).toInt();
        } else if (line.startsWith("TOTAL:")) {
            settings_total = line.mid(6).toInt();
        }
    }

    report += QString("║ Settings Modified: %1 / %2").arg(settings_changed)
        .arg(settings_total).leftJustified(73, ' ') + "║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
}

void DisableVisualEffectsAction::buildAndFinishVisualEffectsResult(
    const QString& report, int settings_total, int settings_changed,
    bool notification_success, const QString& fx_mode,
    const QDateTime& start_time)
{
    QString structured_output;
    structured_output += QString("SETTINGS_TOTAL:%1\n").arg(settings_total);
    structured_output += QString("SETTINGS_CHANGED:%1\n").arg(settings_changed);
    structured_output += QString("SETTINGS_OPTIMIZED:%1\n").arg(settings_total - settings_changed);
    structured_output += QString("NOTIFICATION_SUCCESS:%1\n")
        .arg(notification_success ? "YES" : "NO");
    structured_output += QString("RESTART_REQUIRED:%1\n").arg(settings_changed > 0 ? "YES" : "NO");
    structured_output += QString("VISUAL_FX_MODE:%1\n").arg(fx_mode);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.success = (settings_total > 0);
    result.message = settings_changed > 0
        ? QString("Visual effects optimized (%1 settings changed)").arg(settings_changed)
        : "Visual effects already optimized for Best Performance";
    result.log = report + "\n" + structured_output;

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

// ============================================================================
// Private Helpers
// ============================================================================

QString DisableVisualEffectsAction::buildCheckSettingsScript() const
{
    return R"(
            $settings = @{
                VisualFXSetting = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects' -Name 'VisualFXSetting' -ErrorAction SilentlyContinue).VisualFXSetting
                TaskbarAnimations = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'TaskbarAnimations' -ErrorAction SilentlyContinue).TaskbarAnimations
                EnableAeroPeek = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'EnableAeroPeek' -ErrorAction SilentlyContinue).EnableAeroPeek
                AlwaysHibernateThumbnails = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'AlwaysHibernateThumbnails' -ErrorAction SilentlyContinue).AlwaysHibernateThumbnails
                MinAnimate = (Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name 'MinAnimate' -ErrorAction SilentlyContinue).MinAnimate
                ListviewAlphaSelect = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewAlphaSelect' -ErrorAction SilentlyContinue).ListviewAlphaSelect
                ListviewShadow = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewShadow' -ErrorAction SilentlyContinue).ListviewShadow
                DragFullWindows = (Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'DragFullWindows' -ErrorAction SilentlyContinue).DragFullWindows
                FontSmoothing = (Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'FontSmoothing' -ErrorAction SilentlyContinue).FontSmoothing
            }
            $settings | ConvertTo-Json
        )";
}

QString DisableVisualEffectsAction::buildApplyScriptPerformanceChecks() const
{
    return R"(
            # VisualFXSetting: 2 = Best Performance
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects' -Name 'VisualFXSetting' -ErrorAction SilentlyContinue).VisualFXSetting -ne 2) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects' -Name 'VisualFXSetting' -Value 2 -Type DWord -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'TaskbarAnimations' -ErrorAction SilentlyContinue).TaskbarAnimations -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'TaskbarAnimations' -Value 0 -Type DWord -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'EnableAeroPeek' -ErrorAction SilentlyContinue).EnableAeroPeek -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'EnableAeroPeek' -Value 0 -Type DWord -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'AlwaysHibernateThumbnails' -ErrorAction SilentlyContinue).AlwaysHibernateThumbnails -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'AlwaysHibernateThumbnails' -Value 0 -Type DWord -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name 'MinAnimate' -ErrorAction SilentlyContinue).MinAnimate -ne '0') {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name 'MinAnimate' -Value '0' -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewAlphaSelect' -ErrorAction SilentlyContinue).ListviewAlphaSelect -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewAlphaSelect' -Value 0 -Type DWord -Force
                $changes++
            }
            )";
}

QString DisableVisualEffectsAction::buildApplyScriptDesktopChecks() const
{
    return R"(
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewShadow' -ErrorAction SilentlyContinue).ListviewShadow -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewShadow' -Value 0 -Type DWord -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'DragFullWindows' -ErrorAction SilentlyContinue).DragFullWindows -ne '0') {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'DragFullWindows' -Value '0' -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'FontSmoothing' -ErrorAction SilentlyContinue).FontSmoothing -ne '2') {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'FontSmoothing' -Value '2' -Force
                $changes++
            }

            $total++
            $currentMask = (Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'UserPreferencesMask' -ErrorAction SilentlyContinue).UserPreferencesMask
            $targetMask = [byte[]]@(0x90, 0x32, 0x07, 0x80, 0x10, 0x00, 0x00, 0x00)
            if ($null -eq $currentMask -or (Compare-Object $currentMask $targetMask)) {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'UserPreferencesMask' -Value $targetMask -Type Binary -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'IconsOnly' -ErrorAction SilentlyContinue).IconsOnly -ne 1) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'IconsOnly' -Value 1 -Type DWord -Force
                $changes++
            }

            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ShowInfoTip' -ErrorAction SilentlyContinue).ShowInfoTip -ne 1) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ShowInfoTip' -Value 1 -Type DWord -Force
                $changes++
            }

            Write-Output "CHANGES:$changes"
            Write-Output "TOTAL:$total"
            )";
}

QString DisableVisualEffectsAction::buildApplySettingsScript() const
{
    return "\n            $changes = 0\n            $total = 0\n"
           + buildApplyScriptPerformanceChecks()
           + buildApplyScriptDesktopChecks();
}

QString DisableVisualEffectsAction::buildCurrentSettingsReport(
    const QJsonObject& current_settings) const
{
    int visual_fx = current_settings["VisualFXSetting"].toInt(-1);
    QString fx_mode = (visual_fx == 0) ? "Let Windows choose" :
                      (visual_fx == 1) ? "Best appearance" :
                      (visual_fx == 2) ? "Best performance" :
                      (visual_fx == 3) ? "Custom" : "Unknown";

    QString section;
    section += QString("║ Current Mode: %1").arg(fx_mode).leftJustified(73, ' ') + "║\n";
    section += QString("║ VisualFXSetting: %1").arg(visual_fx).leftJustified(73, ' ') + "║\n";
    section += QString("║ TaskbarAnimations: %1")
        .arg(current_settings["TaskbarAnimations"].toInt(-1)).leftJustified(73, ' ') + "║\n";
    section += QString("║ EnableAeroPeek: %1")
        .arg(current_settings["EnableAeroPeek"].toInt(-1)).leftJustified(73, ' ') + "║\n";
    section += QString("║ AlwaysHibernateThumbnails: %1")
        .arg(current_settings["AlwaysHibernateThumbnails"].toInt(-1))
            .leftJustified(73, ' ') + "║\n";
    section += QString("║ MinAnimate: %1")
        .arg(current_settings["MinAnimate"].toString()).leftJustified(73, ' ') + "║\n";
    section += "╠══════════════════════════════════════════════════════════════════════╣\n";
    return section;
}

QString DisableVisualEffectsAction::buildSummaryReport(int settings_total, int settings_changed,
    const QString& fx_mode) const
{
    Q_UNUSED(fx_mode)

    QString section;
    section += "║ OPTIMIZATION SUMMARY                                                 ║\n";
    section += "╠══════════════════════════════════════════════════════════════════════╣\n";
    section += QString("║ Total Settings: %1").arg(settings_total).leftJustified(73, ' ') + "║\n";
    section += QString("║ Settings Changed: %1").arg(settings_changed).leftJustified(73,
        ' ') + "║\n";
    section += QString("║ Settings Already Optimized: %1")
        .arg(settings_total - settings_changed).leftJustified(73, ' ') + "║\n";
    section += "║                                                                      ║\n";

    if (settings_changed > 0) {
        section += "║ ⚠ RESTART REQUIRED                                                   ║\n";
        section += "║   Log off and log back in to apply all visual effects changes.      ║\n";
        section += "║   Some changes may take effect immediately in new windows.          ║\n";
    } else {
        section += "║ ✓ All visual effects already optimized for Best Performance         ║\n";
    }

    section += "║                                                                      ║\n";
    section += "║ OPTIMIZATIONS APPLIED:                                               ║\n";
    section += "║  • VisualFXSetting = Best Performance (2)                            ║\n";
    section += "║  • TaskbarAnimations = Disabled                                      ║\n";
    section += "║  • EnableAeroPeek = Disabled                                         ║\n";
    section += "║  • AlwaysHibernateThumbnails = Disabled                              ║\n";
    section += "║  • MinAnimate (window animations) = Disabled                         ║\n";
    section += "║  • ListviewAlphaSelect = Disabled                                    ║\n";
    section += "║  • ListviewShadow = Disabled                                         ║\n";
    section += "║  • DragFullWindows = Disabled                                        ║\n";
    section += "║  • FontSmoothing = ClearType (Enabled for readability)               ║\n";
    section += "║  • UserPreferencesMask = Performance optimized                       ║\n";
    section += "║  • IconsOnly = Enabled                                               ║\n";
    section += "║  • ShowInfoTip = Enabled (minimal)                                   ║\n";
    section += "╚══════════════════════════════════════════════════════════════════════╝\n";
    return section;
}

} // namespace sak
