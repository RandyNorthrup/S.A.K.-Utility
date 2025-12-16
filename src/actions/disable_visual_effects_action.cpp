// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/disable_visual_effects_action.h"
#include <QSettings>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace sak {

DisableVisualEffectsAction::DisableVisualEffectsAction(QObject* parent)
    : QuickAction(parent)
{
}

bool DisableVisualEffectsAction::areVisualEffectsEnabled() {
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects", 
                      QSettings::NativeFormat);
    
    int value = settings.value("VisualFXSetting", 2).toInt();
    // 0 = Let Windows choose, 1 = Best appearance, 2 = Best performance, 3 = Custom
    return (value != 2);
}

bool DisableVisualEffectsAction::disableVisualEffects() {
    // Set to "Best Performance"
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects", 
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
    QProcess::execute("rundll32.exe", 
        QStringList() << "user32.dll,UpdatePerUserSystemParameters" << "1" << "True");
    
    return true;
}

void DisableVisualEffectsAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to optimize visual effects";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void DisableVisualEffectsAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    QString report;
    QString structured_output;
    int settings_changed = 0;
    int settings_total = 0;
    
    Q_EMIT executionProgress("Analyzing current visual effects settings...", 10);
    
    // Phase 1: Enumerate current visual effects settings via PowerShell
    report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    report += "║                   VISUAL EFFECTS OPTIMIZATION                        ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ Phase 1: Current Settings Analysis                                  ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    QProcess ps_check;
    ps_check.setProgram("powershell.exe");
    ps_check.setArguments(QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command"
        << R"(
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
        )");
    
    ps_check.start();
    ps_check.waitForFinished(10000);
    QString check_output = QString::fromUtf8(ps_check.readAllStandardOutput()).trimmed();
    
    // Parse current settings
    QJsonDocument check_doc = QJsonDocument::fromJson(check_output.toUtf8());
    QJsonObject current_settings = check_doc.object();
    
    int visual_fx = current_settings["VisualFXSetting"].toInt(-1);
    QString fx_mode = (visual_fx == 0) ? "Let Windows choose" :
                      (visual_fx == 1) ? "Best appearance" :
                      (visual_fx == 2) ? "Best performance" :
                      (visual_fx == 3) ? "Custom" : "Unknown";
    
    report += QString("║ Current Mode: %1").arg(fx_mode).leftJustified(73, ' ') + "║\n";
    report += QString("║ VisualFXSetting: %1").arg(visual_fx).leftJustified(73, ' ') + "║\n";
    report += QString("║ TaskbarAnimations: %1").arg(current_settings["TaskbarAnimations"].toInt(-1)).leftJustified(73, ' ') + "║\n";
    report += QString("║ EnableAeroPeek: %1").arg(current_settings["EnableAeroPeek"].toInt(-1)).leftJustified(73, ' ') + "║\n";
    report += QString("║ AlwaysHibernateThumbnails: %1").arg(current_settings["AlwaysHibernateThumbnails"].toInt(-1)).leftJustified(73, ' ') + "║\n";
    report += QString("║ MinAnimate: %1").arg(current_settings["MinAnimate"].toString()).leftJustified(73, ' ') + "║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    Q_EMIT executionProgress("Applying Best Performance settings...", 35);
    
    // Phase 2: Apply comprehensive Best Performance settings via PowerShell
    report += "║ Phase 2: Applying Best Performance Settings                         ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    QProcess ps_apply;
    ps_apply.setProgram("powershell.exe");
    ps_apply.setArguments(QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command"
        << R"(
            $changes = 0
            $total = 0
            
            # VisualFXSetting: 2 = Best Performance
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects' -Name 'VisualFXSetting' -ErrorAction SilentlyContinue).VisualFXSetting -ne 2) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects' -Name 'VisualFXSetting' -Value 2 -Type DWord -Force
                $changes++
            }
            
            # Disable taskbar animations
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'TaskbarAnimations' -ErrorAction SilentlyContinue).TaskbarAnimations -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'TaskbarAnimations' -Value 0 -Type DWord -Force
                $changes++
            }
            
            # Disable Aero Peek
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'EnableAeroPeek' -ErrorAction SilentlyContinue).EnableAeroPeek -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'EnableAeroPeek' -Value 0 -Type DWord -Force
                $changes++
            }
            
            # Disable thumbnail hibernation
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'AlwaysHibernateThumbnails' -ErrorAction SilentlyContinue).AlwaysHibernateThumbnails -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\DWM' -Name 'AlwaysHibernateThumbnails' -Value 0 -Type DWord -Force
                $changes++
            }
            
            # Disable window minimize/maximize animations
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name 'MinAnimate' -ErrorAction SilentlyContinue).MinAnimate -ne '0') {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop\WindowMetrics' -Name 'MinAnimate' -Value '0' -Force
                $changes++
            }
            
            # Disable listview alpha select
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewAlphaSelect' -ErrorAction SilentlyContinue).ListviewAlphaSelect -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewAlphaSelect' -Value 0 -Type DWord -Force
                $changes++
            }
            
            # Disable listview shadow
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewShadow' -ErrorAction SilentlyContinue).ListviewShadow -ne 0) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ListviewShadow' -Value 0 -Type DWord -Force
                $changes++
            }
            
            # Disable full window dragging (0 = disabled)
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'DragFullWindows' -ErrorAction SilentlyContinue).DragFullWindows -ne '0') {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'DragFullWindows' -Value '0' -Force
                $changes++
            }
            
            # Enable font smoothing (2 = ClearType)
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'FontSmoothing' -ErrorAction SilentlyContinue).FontSmoothing -ne '2') {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'FontSmoothing' -Value '2' -Force
                $changes++
            }
            
            # UserPreferencesMask for advanced performance (hex: 9032078010000000)
            # This controls: animations, shadow effects, menu show delay, etc.
            $total++
            $currentMask = (Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'UserPreferencesMask' -ErrorAction SilentlyContinue).UserPreferencesMask
            $targetMask = [byte[]]@(0x90, 0x32, 0x07, 0x80, 0x10, 0x00, 0x00, 0x00)
            if ($null -eq $currentMask -or (Compare-Object $currentMask $targetMask)) {
                Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name 'UserPreferencesMask' -Value $targetMask -Type Binary -Force
                $changes++
            }
            
            # IconsOnly mode for performance
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'IconsOnly' -ErrorAction SilentlyContinue).IconsOnly -ne 1) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'IconsOnly' -Value 1 -Type DWord -Force
                $changes++
            }
            
            # Disable ShowInfoTip for performance
            $total++
            if ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ShowInfoTip' -ErrorAction SilentlyContinue).ShowInfoTip -ne 1) {
                Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name 'ShowInfoTip' -Value 1 -Type DWord -Force
                $changes++
            }
            
            Write-Output "CHANGES:$changes"
            Write-Output "TOTAL:$total"
        )");
    
    ps_apply.start();
    ps_apply.waitForFinished(15000);
    QString apply_output = QString::fromUtf8(ps_apply.readAllStandardOutput()).trimmed();
    
    // Parse changes count
    for (const QString& line : apply_output.split('\n')) {
        if (line.startsWith("CHANGES:")) {
            settings_changed = line.mid(8).toInt();
        } else if (line.startsWith("TOTAL:")) {
            settings_total = line.mid(6).toInt();
        }
    }
    
    report += QString("║ Settings Modified: %1 / %2").arg(settings_changed).arg(settings_total).leftJustified(73, ' ') + "║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    Q_EMIT executionProgress("Notifying system of changes...", 70);
    
    // Phase 3: Notify system of changes
    report += "║ Phase 3: System Notification                                        ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    bool notification_success = QProcess::execute("rundll32.exe",
        QStringList() << "user32.dll,UpdatePerUserSystemParameters" << "1" << "True") == 0;
    
    report += QString("║ System Notification: %1").arg(notification_success ? "✓ Success" : "✗ Failed").leftJustified(73, ' ') + "║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    Q_EMIT executionProgress("Optimization complete", 100);
    
    // Summary
    report += "║ OPTIMIZATION SUMMARY                                                 ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ Total Settings: %1").arg(settings_total).leftJustified(73, ' ') + "║\n";
    report += QString("║ Settings Changed: %1").arg(settings_changed).leftJustified(73, ' ') + "║\n";
    report += QString("║ Settings Already Optimized: %1").arg(settings_total - settings_changed).leftJustified(73, ' ') + "║\n";
    report += "║                                                                      ║\n";
    
    if (settings_changed > 0) {
        report += "║ ⚠ RESTART REQUIRED                                                   ║\n";
        report += "║   Log off and log back in to apply all visual effects changes.      ║\n";
        report += "║   Some changes may take effect immediately in new windows.          ║\n";
    } else {
        report += "║ ✓ All visual effects already optimized for Best Performance         ║\n";
    }
    
    report += "║                                                                      ║\n";
    report += "║ OPTIMIZATIONS APPLIED:                                               ║\n";
    report += "║  • VisualFXSetting = Best Performance (2)                            ║\n";
    report += "║  • TaskbarAnimations = Disabled                                      ║\n";
    report += "║  • EnableAeroPeek = Disabled                                         ║\n";
    report += "║  • AlwaysHibernateThumbnails = Disabled                              ║\n";
    report += "║  • MinAnimate (window animations) = Disabled                         ║\n";
    report += "║  • ListviewAlphaSelect = Disabled                                    ║\n";
    report += "║  • ListviewShadow = Disabled                                         ║\n";
    report += "║  • DragFullWindows = Disabled                                        ║\n";
    report += "║  • FontSmoothing = ClearType (Enabled for readability)               ║\n";
    report += "║  • UserPreferencesMask = Performance optimized                       ║\n";
    report += "║  • IconsOnly = Enabled                                               ║\n";
    report += "║  • ShowInfoTip = Enabled (minimal)                                   ║\n";
    report += "╚══════════════════════════════════════════════════════════════════════╝\n";
    
    // Structured output
    structured_output += QString("SETTINGS_TOTAL:%1\n").arg(settings_total);
    structured_output += QString("SETTINGS_CHANGED:%1\n").arg(settings_changed);
    structured_output += QString("SETTINGS_OPTIMIZED:%1\n").arg(settings_total - settings_changed);
    structured_output += QString("NOTIFICATION_SUCCESS:%1\n").arg(notification_success ? "YES" : "NO");
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
    
    setStatus(result.success ? ActionStatus::Success : ActionStatus::Failed);
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak
