// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file disable_startup_programs_action.cpp
/// @brief Implements startup program management and disabling

#include "sak/actions/disable_startup_programs_action.h"
#include "sak/process_runner.h"
#include <QSettings>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "sak/layout_constants.h"
#include "sak/logger.h"

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
    ProcessResult proc = runProcess("schtasks", QStringList() << "/Query" << "/FO" << "CSV",
        sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Scheduled task scan warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n');

    for (const QString& line : lines) {
        if (!line.contains("Ready") || !line.contains("\\Microsoft\\Windows")) continue;
        QStringList parts = line.split(',');
        if (parts.isEmpty()) continue;

        StartupItem item;
        item.name = parts[0].trimmed().remove('"');
        item.command = "Scheduled Task";
        item.location = "Task Scheduler";
        item.is_enabled = true;
        item.impact = "Low";

        m_startup_items.append(item);
    }
}

void DisableStartupProgramsAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    m_startup_items.clear();
    scanRegistryStartup();
    scanStartupFolder();
    scanTaskScheduler();

    ScanResult result;
    result.applicable = !m_startup_items.isEmpty();
    result.files_count = m_startup_items.size();
    result.summary = result.applicable
        ? QString("Startup items found: %1").arg(m_startup_items.size())
        : "No startup items detected";
    result.details = "Run analysis to review and disable non-essential items";

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void DisableStartupProgramsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Disable startup programs cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    QString startup_output;
    int startup_count = 0;
    if (!executeScanRegistry(start_time, startup_output, startup_count)) return;

    QString task_output;
    int task_count = 0;
    if (!executeScanTaskScheduler(start_time, task_output, task_count)) return;

    QString report;
    bool tm_launched = false;
    executeDisableEntries(start_time, startup_output, startup_count,
                          task_output, task_count, report, tm_launched);

    executeBuildReport(start_time, startup_count, task_count, report, tm_launched);
}

bool DisableStartupProgramsAction::executeScanRegistry(const QDateTime& start_time,
                                                        QString& startup_output,
                                                        int& startup_count)
{
    Q_UNUSED(start_time)
    Q_EMIT executionProgress("Phase 1: Scanning startup programs...", 10);

    QString startup_scan_cmd =
        R"(Get-CimInstance Win32_StartupCommand )"
        R"(| Select-Object Name, Command, Location, User )"
        R"(| ConvertTo-Json)";

    ProcessResult startup_proc = runPowerShell(startup_scan_cmd, sak::kTimeoutChocoListMs);
    if (!startup_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Startup program scan warning: " + startup_proc.std_err.trimmed());
    }
    startup_output = startup_proc.std_out;

    // Count startup items
    if (startup_output.contains("[")) {
        QJsonDocument doc = QJsonDocument::fromJson(startup_output.toUtf8());
        if (doc.isArray()) {
            startup_count = doc.array().size();
        }
    }

    return true;
}

bool DisableStartupProgramsAction::executeScanTaskScheduler(const QDateTime& start_time,
                                                             QString& task_output,
                                                             int& task_count)
{
    Q_UNUSED(start_time)
    Q_EMIT executionProgress("Phase 2: Scanning scheduled tasks at startup...", 35);

    QString task_scan_cmd =
        R"(Get-ScheduledTask | Where-Object )"
        R"({$_.Triggers.CimClass.CimClassName -match )"
        R"('MSFT_TaskLogonTrigger|MSFT_TaskBootTrigger'} )"
        R"(| Select-Object TaskName, State, TaskPath )"
        R"(| ConvertTo-Json)";

    ProcessResult task_proc = runPowerShell(task_scan_cmd, sak::kTimeoutChocoListMs);
    if (!task_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Startup task scan warning: " + task_proc.std_err.trimmed());
    }
    task_output = task_proc.std_out;

    if (task_output.contains("[") || task_output.contains("{")) {
        QJsonDocument doc = QJsonDocument::fromJson(task_output.toUtf8());
        if (doc.isArray()) {
            task_count = doc.array().size();
        } else if (doc.isObject()) {
            task_count = 1;
        }
    }

    return true;
}

QString DisableStartupProgramsAction::formatStartupProgramsSection(const QString& startup_output,
                                                                     int startup_count) const
{
    if (startup_count <= 0) return {};

    QString section;
    section += QString::fromUtf8("║                         STARTUP PROGRAMS                       "
                                 "            ║\n");
    section += QString::fromUtf8(
        "╠════════════════════════════════════════════════════════════════════════════╣\n");

    QJsonDocument doc = QJsonDocument::fromJson(startup_output.toUtf8());
    QJsonArray programs = doc.array();

    int displayed = 0;
    for (const QJsonValue& value : programs) {
        if (displayed >= 15) break; // Limit to first 15 for readability
        QJsonObject program = value.toObject();
        QString name = program["Name"].toString().left(50);
        QString location = program["Location"].toString();

        // Determine location type for icon
        QString icon = QString::fromUtf8("●");
        if (location.contains("HKLM", Qt::CaseInsensitive)) {
            icon = QString::fromUtf8("■"); // System-wide
        } else if (location.contains("HKCU", Qt::CaseInsensitive)) {
            icon = QString::fromUtf8("□"); // User-specific
        } else if (location.contains("Startup", Qt::CaseInsensitive)) {
            icon = QString::fromUtf8("▸"); // Startup folder
        }

        section += QString::fromUtf8("║ %1 %2").arg(icon).arg(name).leftJustified(73,
            ' ') + QString::fromUtf8("║\n");

        QString loc_short = location.left(60);
        if (!loc_short.isEmpty()) {
            section += QString::fromUtf8("║   Location: %1").arg(loc_short).leftJustified(73,
                ' ') + QString::fromUtf8("║\n");
        }
        displayed++;
    }

    if (programs.size() > 15) {
        section += QString::fromUtf8("║   ... and %1 more startup program(s)                       "
                                     "               ║\n")
                     .arg(programs.size() - 15);
    }
    section += QString::fromUtf8(
        "╠════════════════════════════════════════════════════════════════════════════╣\n");
    return section;
}

QString DisableStartupProgramsAction::formatStartupTasksSection(const QString& task_output,
                                                                  int task_count) const
{
    if (task_count <= 0) return {};

    QString section;
    section += QString::fromUtf8("║                         STARTUP TASKS                          "
                                 "            ║\n");
    section += QString::fromUtf8(
        "╠════════════════════════════════════════════════════════════════════════════╣\n");

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

        QString state_icon = (state == "Ready") ? QString::fromUtf8("✓") : QString::fromUtf8("◯");

        section += QString::fromUtf8("║ %1 %2").arg(state_icon).arg(name).leftJustified(73,
            ' ') + QString::fromUtf8("║\n");
        section += QString::fromUtf8("║   State: %1").arg(state).leftJustified(73,
            ' ') + QString::fromUtf8("║\n");
        displayed++;
    }

    if (tasks.size() > 10) {
        section += QString::fromUtf8("║   ... and %1 more startup task(s)                          "
                                     "               ║\n")
                     .arg(tasks.size() - 10);
    }
    section += QString::fromUtf8(
        "╠════════════════════════════════════════════════════════════════════════════╣\n");
    return section;
}

void DisableStartupProgramsAction::executeDisableEntries(const QDateTime& start_time,
                                                          const QString& startup_output,
                                                          int startup_count,
                                                          const QString& task_output,
                                                          int task_count,
                                                          QString& report,
                                                          bool& tm_launched)
{
    Q_UNUSED(start_time)
    Q_EMIT executionProgress("Phase 3: Analyzing impact and generating report...", 60);

    // Generate enterprise box-drawing report
    report += "╔════════════════════════════════════════════════════════════════════════════╗\n";
    report += "║                    STARTUP PROGRAMS ANALYSIS REPORT                        ║\n";
    report += "╠════════════════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ Scan Time:              %1                               ║\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    report += QString("║ Startup Programs Found: %1                                                "
                      " ║\n").arg(startup_count);
    report += QString("║ Startup Tasks Found:    %1                                                "
                      "  ║\n").arg(task_count);
    report += QString("║ Total Startup Items:    %1                                                "
                      " ║\n").arg(startup_count + task_count);
    report += "╠════════════════════════════════════════════════════════════════════════════╣\n";

    // Parse and display startup programs
    report += formatStartupProgramsSection(startup_output, startup_count);

    // Parse and display startup tasks
    report += formatStartupTasksSection(task_output, task_count);

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
        report += "║ ⚠ High startup item count detected                                        ║\n";
        report += "║   Consider disabling unnecessary programs to improve boot time            ║\n";
    }
    if (startup_count + task_count > 25) {
        report += "║ ⚠ Very high startup load - boot performance likely impacted               ║\n";
    }

    report += "║                                                                            ║\n";
    report += "║ Management Options:                                                        ║\n";
    report += "║ • Use Task Manager > Startup tab (Ctrl+Shift+Esc)                         ║\n";
    report += "║ • Registry: HKEY_CURRENT_USER\\...\\Run                                      ║\n";
    report += "║ • Registry: HKEY_LOCAL_MACHINE\\...\\Run                                     ║\n";
    report += "║ • Startup folder: shell:startup                                            ║\n";
    report += "║ • Task Scheduler: taskschd.msc                                             ║\n";
    report += "╚════════════════════════════════════════════════════════════════════════════╝\n";

    // Launch Task Manager to Startup tab
    Q_EMIT executionProgress("Phase 4: Opening Task Manager...", 85);

    QProcess taskmgr;
    tm_launched = taskmgr.startDetached("taskmgr.exe", QStringList() << "/0" << "/startup");
}

void DisableStartupProgramsAction::executeBuildReport(const QDateTime& start_time,
                                                       int startup_count,
                                                       int task_count,
                                                       const QString& report,
                                                       bool tm_launched)
{
    Q_EMIT executionProgress("Analysis complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    // Structured output for external processing
    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
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

    Q_ASSERT(result.duration_ms >= 0);

    finishWithResult(result, ActionStatus::Success);
}

} // namespace sak
