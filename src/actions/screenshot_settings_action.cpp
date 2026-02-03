// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/screenshot_settings_action.h"
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
    QThread::msleep(2000);
    
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
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Detecting monitor configuration...", 3);
    
    // Phase 1: Detect monitor configuration for enterprise reporting
    int monitor_count = detectMonitorCount();
    
    Q_EMIT executionProgress("Preparing screenshot directory...", 5);
    
    // Create timestamped output directory for this session
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QDir output_dir(m_output_location + "/SettingsScreenshots/" + timestamp);
    output_dir.mkpath(".");
    
    int screenshots_taken = 0;
    int failed_attempts = 0;
    QStringList captured_pages;
    QStringList failed_pages;
    
    // Phase 2: Enhanced Settings pages with retry logic
    QMap<QString, QString> settings_pages = {
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
    
    int processed = 0;
    for (auto it = settings_pages.begin(); it != settings_pages.end(); ++it) {
        if (isCancelled()) {
            ProcessResult kill_proc = runProcess("taskkill", QStringList() << "/IM" << "SystemSettings.exe" << "/F", 10000);
            if (kill_proc.timed_out || kill_proc.exit_code != 0) {
                Q_EMIT logMessage("Settings close warning: " + kill_proc.std_err.trimmed());
            }
            ExecutionResult result;
            result.success = false;
            result.message = "Settings screenshots cancelled";
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            setStatus(ActionStatus::Cancelled);
            Q_EMIT executionComplete(result);
            return;
        }
        
        int progress = 5 + (processed * 90) / settings_pages.size();
        Q_EMIT executionProgress(QString("Capturing %1...").arg(it.value()), progress);
        
        // Phase 3: Enhanced capture with retry logic
        QString page_name = it.value();
        QString ms_uri = it.key();
        bool capture_success = false;
        
        // Try up to 3 times with increasing delays
        for (int attempt = 1; attempt <= 3 && !capture_success; attempt++) {
            // Open Windows Settings
            QProcess::startDetached("explorer.exe", QStringList() << QString("ms-settings:%1").arg(ms_uri));
            
            // Wait for window to open with retry-dependent delay
            int wait_time = 2000 + (attempt - 1) * 1000; // 2s, 3s, 4s
            QThread::msleep(wait_time);
            
            // Verify Settings window exists before capture
            bool settings_running = isProcessRunning("SystemSettings.exe");
            
            if (settings_running) {
                // Capture screenshot with timestamped filename
                QString filepath = output_dir.filePath(QString("%1_%2.png").arg(page_name).arg(timestamp));
                
                // Multi-monitor support: capture all screens
                QList<QScreen*> screens = QGuiApplication::screens();
                if (screens.size() > 1) {
                    // Capture all monitors
                    for (int i = 0; i < screens.size(); i++) {
                        QScreen* screen = screens[i];
                        QPixmap screenshot = screen->grabWindow(0);
                        QString multi_filepath = output_dir.filePath(QString("%1_Monitor%2_%3.png").arg(page_name).arg(i+1).arg(timestamp));
                        if (screenshot.save(multi_filepath, "PNG")) {
                            capture_success = true;
                            screenshots_taken++;
                        }
                    }
                } else {
                    // Single monitor capture
                    QScreen* screen = QGuiApplication::primaryScreen();
                    if (screen) {
                        QPixmap screenshot = screen->grabWindow(0);
                        if (screenshot.save(filepath, "PNG")) {
                            capture_success = true;
                            screenshots_taken++;
                        }
                    }
                }
            }
            
            // Close Settings window for next iteration
            ProcessResult close_proc = runProcess("taskkill", QStringList() << "/IM" << "SystemSettings.exe" << "/F", 10000);
            if (close_proc.timed_out || close_proc.exit_code != 0) {
                Q_EMIT logMessage("Settings close warning: " + close_proc.std_err.trimmed());
            }
            QThread::msleep(500);
        }
        
        if (capture_success) {
            captured_pages.append(page_name);
        } else {
            failed_attempts++;
            failed_pages.append(page_name);
        }
        
        processed++;
    }
    
    Q_EMIT executionProgress("Generating report...", 95);
    
    // Phase 4: Generate enterprise box-drawing report
    QString report_path = output_dir.filePath(QString("Screenshot_Report_%1.txt").arg(timestamp));
    QFile report_file(report_path);
    if (report_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream report(&report_file);
        report.setEncoding(QStringConverter::Utf8);
        
        report << "╔══════════════════════════════════════════════════════════════╗\n";
        report << "║         WINDOWS SETTINGS SCREENSHOT REPORT                   ║\n";
        report << "╠══════════════════════════════════════════════════════════════╣\n";
        report << QString("║ Timestamp:         %1                    ║\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        report << QString("║ Monitors Detected: %1                                       ║\n").arg(monitor_count);
        report << QString("║ Total Pages:       %1                                      ║\n").arg(settings_pages.size());
        report << QString("║ Successful:        %1                                      ║\n").arg(captured_pages.size());
        report << QString("║ Failed:            %1                                       ║\n").arg(failed_attempts);
        report << "╠══════════════════════════════════════════════════════════════╣\n";
        report << "║                    CAPTURED PAGES                            ║\n";
        report << "╠══════════════════════════════════════════════════════════════╣\n";
        
        for (const QString& page : captured_pages) {
            report << QString("║ ✓ %1").arg(page).leftJustified(61, ' ') << "║\n";
        }
        
        if (!failed_pages.isEmpty()) {
            report << "╠══════════════════════════════════════════════════════════════╣\n";
            report << "║                     FAILED PAGES                             ║\n";
            report << "╠══════════════════════════════════════════════════════════════╣\n";
            for (const QString& page : failed_pages) {
                report << QString("║ ✗ %1").arg(page).leftJustified(61, ' ') << "║\n";
            }
        }
        
        report << "╠══════════════════════════════════════════════════════════════╣\n";
        report << QString("║ Output Location: %1").arg(output_dir.absolutePath()).leftJustified(61, ' ') << "║\n";
        report << "╚══════════════════════════════════════════════════════════════╝\n";
        
        report_file.close();
    }
    
    Q_EMIT executionProgress("Screenshots complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    // Phase 5: Structured output for external processing
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = screenshots_taken;
    result.output_path = output_dir.absolutePath();
    
    // Add structured data to log
    QString structured_log;
    structured_log += QString("MONITORS_DETECTED:%1\n").arg(monitor_count);
    structured_log += QString("SUCCESSFUL_CAPTURES:%1\n").arg(captured_pages.size());
    structured_log += QString("FAILED_CAPTURES:%1\n").arg(failed_attempts);
    structured_log += QString("TOTAL_PAGES:%1\n").arg(settings_pages.size());
    structured_log += QString("SUCCESS_RATE:%1%\n").arg(captured_pages.size() * 100 / settings_pages.size());
    structured_log += QString("REPORT_PATH:%1\n").arg(report_path);
    
    if (screenshots_taken > 0) {
        result.success = true;
        result.message = QString("Captured %1/%2 settings pages (%3 monitors detected)")
                            .arg(captured_pages.size())
                            .arg(settings_pages.size())
                            .arg(monitor_count);
        result.log = structured_log + QString("\nSaved to: %1").arg(output_dir.absolutePath());
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to capture any screenshots";
        result.log = structured_log + "\nCheck display permissions and Settings app availability";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

// Helper method: Detect monitor count for multi-monitor support
int ScreenshotSettingsAction::detectMonitorCount() {
    QList<QScreen*> screens = QGuiApplication::screens();
    int count = screens.size();
    
    qDebug() << "Detected" << count << "monitor(s)";
    
    // Log each monitor's properties
    for (int i = 0; i < count; i++) {
        QScreen* screen = screens[i];
        QRect geometry = screen->geometry();
        qDebug() << "Monitor" << (i+1) << ":" 
                 << geometry.width() << "x" << geometry.height()
                 << "at" << geometry.x() << "," << geometry.y();
    }
    
    return count;
}

// Helper method: Check if a process is currently running
bool ScreenshotSettingsAction::isProcessRunning(const QString& process_name) {
    QProcess tasklist;
    tasklist.start("tasklist", QStringList() << "/FI" << QString("IMAGENAME eq %1").arg(process_name));
    tasklist.waitForFinished(2000);
    
    QString output = tasklist.readAllStandardOutput();
    return output.contains(process_name, Qt::CaseInsensitive);
}

} // namespace sak
