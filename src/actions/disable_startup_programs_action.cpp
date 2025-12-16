// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/disable_startup_programs_action.h"
#include <QSettings>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace sak {

DisableStartupProgramsAction::DisableStartupProgramsAction(QObject* parent)
    : QuickAction(parent)
{
}

void DisableStartupProgramsAction::scanRegistryStartup() {
    QStringList reg_paths = {
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
    };
    
    for (const QString& path : reg_paths) {
        QSettings settings(path, QSettings::NativeFormat);
        
        for (const QString& key : settings.allKeys()) {
            StartupItem item;
            item.name = key;
            item.command = settings.value(key).toString();
            item.location = path;
            item.is_enabled = true;
            item.impact = "Medium"; // Default
            
            m_startup_items.append(item);
        }
    }
}

void DisableStartupProgramsAction::scanStartupFolder() {
    QString startup_path = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) 
                          + "/../Microsoft/Windows/Start Menu/Programs/Startup";
    
    QDir startup_dir(startup_path);
    if (startup_dir.exists()) {
        for (const QFileInfo& file : startup_dir.entryInfoList(QDir::Files)) {
            StartupItem item;
            item.name = file.baseName();
            item.command = file.absoluteFilePath();
            item.location = "Startup Folder";
            item.is_enabled = true;
            item.impact = "Low";
            
            m_startup_items.append(item);
        }
    }
}

void DisableStartupProgramsAction::scanTaskScheduler() {
    QProcess proc;
    proc.start("schtasks", QStringList() << "/Query" << "/FO" << "CSV");
    proc.waitForFinished(5000);
    
    QString output = proc.readAllStandardOutput();
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        if (line.contains("Ready") && line.contains("\\Microsoft\\Windows")) {
            QStringList parts = line.split(',');
            if (parts.size() > 0) {
                StartupItem item;
                item.name = parts[0].trimmed().remove('"');
                item.command = "Scheduled Task";
                item.location = "Task Scheduler";
                item.is_enabled = true;
                item.impact = "Low";
                
                m_startup_items.append(item);
            }
        }
    }
}

void DisableStartupProgramsAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to manage startup programs";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void DisableStartupProgramsAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Phase 1: Scanning startup programs...", 10);
    
    // Phase 1: Get comprehensive startup items using Win32_StartupCommand
    QString startup_scan_cmd = R"(Get-CimInstance Win32_StartupCommand | Select-Object Name, Command, Location, User | ConvertTo-Json)";
    
    QProcess startup_proc;
    startup_proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << startup_scan_cmd);
    startup_proc.waitForFinished(15000);
    
    QString startup_output = startup_proc.readAllStandardOutput();
    int startup_count = 0;
    
    // Count startup items
    if (startup_output.contains("[")) {
        QJsonDocument doc = QJsonDocument::fromJson(startup_output.toUtf8());
        if (doc.isArray()) {
            startup_count = doc.array().size();
        }
    }
    
    Q_EMIT executionProgress("Phase 2: Scanning scheduled tasks at startup...", 35);
    
    // Phase 2: Get scheduled tasks that run at startup
    QString task_scan_cmd = R"(Get-ScheduledTask | Where-Object {$_.Triggers.CimClass.CimClassName -match 'MSFT_TaskLogonTrigger|MSFT_TaskBootTrigger'} | Select-Object TaskName, State, TaskPath | ConvertTo-Json)";
    
    QProcess task_proc;
    task_proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << task_scan_cmd);
    task_proc.waitForFinished(15000);
    
    QString task_output = task_proc.readAllStandardOutput();
    int task_count = 0;
    
    if (task_output.contains("[") || task_output.contains("{")) {
        QJsonDocument doc = QJsonDocument::fromJson(task_output.toUtf8());
        if (doc.isArray()) {
            task_count = doc.array().size();
        } else if (doc.isObject()) {
            task_count = 1;
        }
    }
    
    Q_EMIT executionProgress("Phase 3: Analyzing impact and generating report...", 60);
    
    // Phase 3: Generate enterprise box-drawing report
    QString report;
    report += "╔════════════════════════════════════════════════════════════════════════════╗\n";
    report += "║                    STARTUP PROGRAMS ANALYSIS REPORT                        ║\n";
    report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ Scan Time:              %1                               ║\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    report += QString("║ Startup Programs Found: %1                                                 ║\n").arg(startup_count);
    report += QString("║ Startup Tasks Found:    %1                                                  ║\n").arg(task_count);
    report += QString("║ Total Startup Items:    %1                                                 ║\n").arg(startup_count + task_count);
    report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    
    // Parse and display startup programs
    if (startup_count > 0) {
        report += "║                         STARTUP PROGRAMS                                   ║\n";
        report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
        
        QJsonDocument doc = QJsonDocument::fromJson(startup_output.toUtf8());
        QJsonArray programs = doc.array();
        
        int displayed = 0;
        for (const QJsonValue& value : programs) {
            if (displayed >= 15) break; // Limit to first 15 for readability
            QJsonObject program = value.toObject();
            QString name = program["Name"].toString().left(50);
            QString location = program["Location"].toString();
            
            // Determine location type for icon
            QString icon = "●";
            if (location.contains("HKLM", Qt::CaseInsensitive)) {
                icon = "■"; // System-wide
            } else if (location.contains("HKCU", Qt::CaseInsensitive)) {
                icon = "□"; // User-specific
            } else if (location.contains("Startup", Qt::CaseInsensitive)) {
                icon = "▸"; // Startup folder
            }
            
            report += QString("║ %1 %2").arg(icon).arg(name).leftJustified(73, ' ') + "║\n";
            
            QString loc_short = location.left(60);
            if (!loc_short.isEmpty()) {
                report += QString("║   Location: %1").arg(loc_short).leftJustified(73, ' ') + "║\n";
            }
            displayed++;
        }
        
        if (programs.size() > 15) {
            report += QString("║   ... and %1 more startup program(s)                                      ║\n")
                         .arg(programs.size() - 15);
        }
        report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    }
    
    // Parse and display startup tasks
    if (task_count > 0) {
        report += "║                         STARTUP TASKS                                      ║\n";
        report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
        
        QJsonDocument doc = QJsonDocument::fromJson(task_output.toUtf8());
        QJsonArray tasks;
        if (doc.isArray()) {
            tasks = doc.array();
        } else if (doc.isObject()) {
            tasks.append(doc.object());
        }
        
        int displayed = 0;
        for (const QJsonValue& value : tasks) {
            if (displayed >= 10) break;
            QJsonObject task = value.toObject();
            QString name = task["TaskName"].toString().left(50);
            QString state = task["State"].toString();
            
            QString state_icon = (state == "Ready") ? "✓" : "◯";
            
            report += QString("║ %1 %2").arg(state_icon).arg(name).leftJustified(73, ' ') + "║\n";
            report += QString("║   State: %1").arg(state).leftJustified(73, ' ') + "║\n";
            displayed++;
        }
        
        if (tasks.size() > 10) {
            report += QString("║   ... and %1 more startup task(s)                                         ║\n")
                         .arg(tasks.size() - 10);
        }
        report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    }
    
    // Legend and recommendations
    report += "║                              LEGEND                                        ║\n";
    report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    report += "║ ■ System-wide (HKLM) - Affects all users                                  ║\n";
    report += "║ □ User-specific (HKCU) - Current user only                                ║\n";
    report += "║ ▸ Startup folder - Easily managed                                         ║\n";
    report += "║ ● Other location                                                          ║\n";
    report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    report += "║                            RECOMMENDATIONS                                 ║\n";
    report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    
    if (startup_count + task_count > 15) {
        report += "║ ⚠ High startup item count detected                                         ║\n";
        report += "║   Consider disabling unnecessary programs to improve boot time            ║\n";
    }
    if (startup_count + task_count > 25) {
        report += "║ ⚠ Very high startup load - boot performance likely impacted                ║\n";
    }
    
    report += "║                                                                            ║\n";
    report += "║ Management Options:                                                        ║\n";
    report += "║ • Use Task Manager > Startup tab (Ctrl+Shift+Esc)                         ║\n";
    report += "║ • Registry: HKEY_CURRENT_USER\\...\\Run                                      ║\n";
    report += "║ • Registry: HKEY_LOCAL_MACHINE\\...\\Run                                     ║\n";
    report += "║ • Startup folder: shell:startup                                            ║\n";
    report += "║ • Task Scheduler: taskschd.msc                                             ║\n";
    report += "╚════════════════════════════════════════════════════════════════════════════╝\n";
    
    Q_EMIT executionProgress("Phase 4: Opening Task Manager...", 85);
    
    // Phase 4: Launch Task Manager to Startup tab
    QProcess taskmgr;
    bool tm_launched = taskmgr.startDetached("taskmgr.exe", QStringList() << "/0" << "/startup");
    
    Q_EMIT executionProgress("Analysis complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    // Phase 5: Structured output for external processing
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = startup_count + task_count;
    
    QString structured_log;
    structured_log += QString("STARTUP_PROGRAMS:%1\n").arg(startup_count);
    structured_log += QString("STARTUP_TASKS:%1\n").arg(task_count);
    structured_log += QString("TOTAL_STARTUP_ITEMS:%1\n").arg(startup_count + task_count);
    structured_log += QString("TASK_MANAGER_LAUNCHED:%1\n").arg(tm_launched ? "YES" : "NO");
    
    int total_items = startup_count + task_count;
    result.success = true;
    result.message = QString("Found %1 startup item(s) - Task Manager opened").arg(total_items);
    result.log = structured_log + "\n" + report;
    
    setExecutionResult(result);
    setStatus(ActionStatus::Success);
    Q_EMIT executionComplete(result);
}

} // namespace sak
