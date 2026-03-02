// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file screenshot_settings_action.cpp
/// @brief Implements screenshot capture and display settings backup

#include "sak/actions/screenshot_settings_action.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"
#include <QProcess>
#include <QThread>
#include <QDir>
#include <QDateTime>
#include <QScreen>
#include <QGuiApplication>

namespace sak {

ScreenshotSettingsAction::ScreenshotSettingsAction(const QString& output_location, QObject* parent)
    : QuickAction(parent)
    , m_output_location(output_location)
{
}

void ScreenshotSettingsAction::captureScreen(const QString& filename) {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return;

    QPixmap screenshot = screen->grabWindow(0);
    screenshot.save(filename, "PNG");
}

void ScreenshotSettingsAction::openSettingsAndCapture(const QString& uri, const QString& name) {
    // Open Windows Settings
    QProcess::startDetached("explorer.exe", QStringList() << QString("ms-settings:%1").arg(uri));

    // Wait for window to open
    QThread::msleep(sak::kTimerServiceDelayMs);

    // Capture screenshot
    QString filepath = m_output_location + "/SettingsScreenshots/" + name + ".png";
    captureScreen(filepath);

    m_screenshots_taken++;
}

void ScreenshotSettingsAction::scan() {
    setStatus(ActionStatus::Scanning);

    ScanResult result;
    result.applicable = true;
    result.summary = "Settings screenshots will open and capture key pages";
    result.details = "Requires interactive desktop session";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ScreenshotSettingsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Screenshot capture cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    Q_EMIT executionProgress("Detecting monitor configuration...", 3);
    int monitor_count = detectMonitorCount();

    Q_EMIT executionProgress("Preparing screenshot directory...", 5);
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QDir output_dir(m_output_location + "/SettingsScreenshots/" + timestamp);
    output_dir.mkpath(".");

    // Capture all settings pages
    QMap<QString, QString> settings_pages = buildSettingsPageMap();
    CaptureResult capture;
    int processed = 0;

    for (auto it = settings_pages.begin(); it != settings_pages.end(); ++it) {
        if (isCancelled()) {
            closeSettingsApp();
            emitCancelledResult("Settings screenshots cancelled", start_time);
            return;
        }

        int progress = 5 + (processed * 90) / settings_pages.size();
        Q_EMIT executionProgress(QString("Capturing %1...").arg(it.value()), progress);

        if (captureSettingsPage(it.key(), it.value(), output_dir.absolutePath(), timestamp)) {
            capture.captured_pages.append(it.value());
            capture.screenshots_taken++;
        } else {
            capture.failed_pages.append(it.value());
            capture.failed_attempts++;
        }
        processed++;
    }

    Q_EMIT executionProgress("Generating report...", 95);
    generateReport(output_dir.absolutePath(), timestamp, monitor_count, capture);
    Q_EMIT executionProgress("Screenshots complete", 100);

    buildExecutionResult(capture, settings_pages.size(), output_dir, monitor_count,
                         timestamp, start_time);
}

void ScreenshotSettingsAction::buildExecutionResult(
        const CaptureResult& capture, int total_pages,
        const QDir& output_dir, int monitor_count,
        const QString& timestamp, const QDateTime& start_time)
{
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = capture.screenshots_taken;
    result.output_path = output_dir.absolutePath();

    QString structured_log;
    structured_log += QString("MONITORS_DETECTED:%1\n").arg(monitor_count);
    structured_log += QString("SUCCESSFUL_CAPTURES:%1\n").arg(capture.captured_pages.size());
    structured_log += QString("FAILED_CAPTURES:%1\n").arg(capture.failed_attempts);
    structured_log += QString("TOTAL_PAGES:%1\n").arg(total_pages);
    structured_log += QString("SUCCESS_RATE:%1%\n")
        .arg(capture.captured_pages.size() * 100 / total_pages);
    structured_log += QString("REPORT_PATH:%1\n")
        .arg(output_dir.filePath(QString("Screenshot_Report_%1.txt").arg(timestamp)));

    if (capture.screenshots_taken > 0) {
        result.success = true;
        result.message = QString("Captured %1/%2 settings pages (%3 monitors detected)")
                            .arg(capture.captured_pages.size())
                            .arg(total_pages)
                            .arg(monitor_count);
        result.log = structured_log + QString("\nSaved to: %1").arg(output_dir.absolutePath());
    } else {
        result.success = false;
        result.message = "Failed to capture any screenshots";
        result.log = structured_log + "\nCheck display permissions and Settings app availability";
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

// ─── Private Helpers ────────────────────────────────────────────────────────────

QMap<QString, QString> ScreenshotSettingsAction::buildSettingsPageMap() {
    return {
        {"about", "System_About"},
        {"network", "Network_Status"},
        {"display", "Display_Settings"},
        {"privacy", "Privacy_General"},
        {"windowsupdate", "Windows_Update"},
        {"activation", "System_Activation"},
        {"network-wifi", "WiFi_Settings"},
        {"network-ethernet", "Ethernet_Settings"},
        {"personalization", "Personalization"},
        {"apps-features", "Installed_Apps"},
        {"powersleep", "Power_Sleep"},
        {"storagesense", "Storage_Settings"},
        {"sound", "Sound_Settings"},
        {"notifications", "Notifications"},
        {"gaming", "Gaming_Settings"}
    };
}

void ScreenshotSettingsAction::closeSettingsApp() {
    ProcessResult close_proc = runProcess("taskkill",
        QStringList() << "/IM" << "SystemSettings.exe" << "/F",
        sak::kTimeoutProcessMediumMs);
    if (!close_proc.succeeded()) {
        Q_EMIT logMessage("Settings close warning: " + close_proc.std_err.trimmed());
    }
}

bool ScreenshotSettingsAction::captureAllScreens(const QDir& output_dir,
                                                   const QString& page_name,
                                                   const QString& timestamp) {
    QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.size() <= 1) {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) return false;
        QPixmap screenshot = screen->grabWindow(0);
        QString filepath = output_dir.filePath(QString("%1_%2.png").arg(page_name, timestamp));
        return screenshot.save(filepath, "PNG");
    }

    bool captured = false;
    for (int i = 0; i < screens.size(); i++) {
        QPixmap screenshot = screens[i]->grabWindow(0);
        QString filepath = output_dir.filePath(
            QString("%1_Monitor%2_%3.png").arg(page_name).arg(i + 1).arg(timestamp));
        if (screenshot.save(filepath, "PNG")) {
            captured = true;
        }
    }
    return captured;
}

bool ScreenshotSettingsAction::captureSettingsPage(const QString& ms_uri,
                                                     const QString& page_name,
                                                     const QString& output_dir_path,
                                                     const QString& timestamp) {
    QDir output_dir(output_dir_path);
    for (int attempt = 1; attempt <= 3; attempt++) {
        QProcess::startDetached("explorer.exe",
            QStringList() << QString("ms-settings:%1").arg(ms_uri));

        int wait_time = 2000 + (attempt - 1) * 1000;
        QThread::msleep(wait_time);

        if (!isProcessRunning("SystemSettings.exe")) {
            closeSettingsApp();
            QThread::msleep(sak::kTimerRetryBaseMs);
            continue;
        }

        bool captured = captureAllScreens(output_dir, page_name, timestamp);
        closeSettingsApp();
        QThread::msleep(sak::kTimerRetryBaseMs);

        if (captured) return true;
    }
    return false;
}

void ScreenshotSettingsAction::generateReport(const QString& output_dir_path,
                                                const QString& timestamp,
                                                int monitor_count,
                                                const CaptureResult& capture) const {
    QDir output_dir(output_dir_path);
    int total_pages = capture.captured_pages.size() + capture.failed_pages.size();
    QString report_path = output_dir.filePath(QString("Screenshot_Report_%1.txt").arg(timestamp));
    QFile report_file(report_path);
    if (!report_file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream report(&report_file);
    report.setEncoding(QStringConverter::Utf8);

    report << "╔══════════════════════════════════════════════════════════════╗\n";
    report << "║         WINDOWS SETTINGS SCREENSHOT REPORT                   ║\n";
    report << "╠══════════════════════════════════════════════════════════════╣\n";
    report
        << QString("║ Timestamp:         %1                    ║\n")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    report
        << QString("║ Monitors Detected: %1                                       ║\n")
            .arg(monitor_count);
    report
        << QString("║ Total Pages:       %1                                      ║\n")
            .arg(total_pages);
    report
        << QString("║ Successful:        %1                                      ║\n")
            .arg(capture.captured_pages.size());
    report
        << QString("║ Failed:            %1                                       ║\n")
            .arg(capture.failed_attempts);
    report << "╠══════════════════════════════════════════════════════════════╣\n";
    report << "║                    CAPTURED PAGES                            ║\n";
    report << "╠══════════════════════════════════════════════════════════════╣\n";

    for (const QString& page : capture.captured_pages) {
        report << QString("║ ✓ %1").arg(page).leftJustified(61, ' ') << "║\n";
    }

    if (!capture.failed_pages.isEmpty()) {
        report << "╠══════════════════════════════════════════════════════════════╣\n";
        report << "║                     FAILED PAGES                             ║\n";
        report << "╠══════════════════════════════════════════════════════════════╣\n";
        for (const QString& page : capture.failed_pages) {
            report << QString("║ ✗ %1").arg(page).leftJustified(61, ' ') << "║\n";
        }
    }

    report << "╠══════════════════════════════════════════════════════════════╣\n";
    report << QString("║ Output Location: %1").arg(output_dir.absolutePath()).leftJustified(61,
        ' ') << "║\n";
    report << "╚══════════════════════════════════════════════════════════════╝\n";

    report_file.close();
}

// Helper method: Detect monitor count for multi-monitor support
int ScreenshotSettingsAction::detectMonitorCount() {
    QList<QScreen*> screens = QGuiApplication::screens();
    int count = screens.size();

    sak::logDebug("Detected {} monitor(s)", count);

    // Log each monitor's properties
    for (int i = 0; i < count; i++) {
        QScreen* screen = screens[i];
        QRect geometry = screen->geometry();
        sak::logDebug("Monitor {}: {}x{} at {},{}",
                     i + 1, geometry.width(), geometry.height(),
                     geometry.x(), geometry.y());
    }

    return count;
}

// Helper method: Check if a process is currently running
bool ScreenshotSettingsAction::isProcessRunning(const QString& process_name) {
    QProcess tasklist;
    tasklist.start("tasklist",
        QStringList() << "/FI" << QString("IMAGENAME eq %1").arg(process_name));
    if (!tasklist.waitForFinished(sak::kTimerServiceDelayMs)) {
        sak::logWarning("tasklist timed out checking for process: {}",
                        process_name.toStdString());
        tasklist.kill();
        return false;
    }

    if (tasklist.exitCode() != 0) {
        sak::logDebug("tasklist returned exit code {} for process: {}",
                      tasklist.exitCode(), process_name.toStdString());
        return false;
    }

    QString output = tasklist.readAllStandardOutput();
    return output.contains(process_name, Qt::CaseInsensitive);
}

} // namespace sak
