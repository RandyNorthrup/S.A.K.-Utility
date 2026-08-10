// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file screenshot_settings_action.cpp
/// @brief Implements screenshot capture and display settings backup

#include "sak/actions/screenshot_settings_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QTextStream>
#include <QThread>

#include <Windows.h>

namespace sak {

namespace {
constexpr int kSettingsCaptureFirstAttempt = 1;
constexpr int kSettingsCaptureMaxAttempts = 3;
constexpr int kSettingsCaptureInitialWaitMs = kTimerStatusBriefMs;
constexpr int kSettingsCaptureRetryWaitMs = kTimerProgressPollMs;
constexpr int kScreenshotReportFieldWidth = 61;
}  // namespace

ScreenshotSettingsAction::ScreenshotSettingsAction(const QString& output_location, QObject* parent)
    : QuickAction(parent), m_output_location(output_location) {}

bool ScreenshotSettingsAction::grabAndSaveOnGui(WId window, const QString& filepath) {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return false;
    }
    const QPixmap shot = screen->grabWindow(window);  // 0 = the whole primary screen
    if (shot.isNull()) {
        return false;
    }
    return shot.save(filepath, "PNG");
}

bool ScreenshotSettingsAction::captureWindowToPng(WId window, const QString& filepath) {
    // QScreen::grabWindow has GUI-thread affinity, but execute() runs on the quick-action worker
    // thread. Marshal the grab onto the GUI thread and block until it finishes. Fail closed if
    // there is no application object or the invocation cannot be marshalled.
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        sak::logError("ScreenshotSettingsAction: no QCoreApplication to marshal capture onto");
        return false;
    }
    if (QThread::currentThread() == app->thread()) {
        return grabAndSaveOnGui(window, filepath);
    }
    bool ok = false;
    const bool invoked = QMetaObject::invokeMethod(
        app,
        [window, filepath, &ok]() { ok = grabAndSaveOnGui(window, filepath); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        sak::logError("ScreenshotSettingsAction: failed to marshal capture onto the GUI thread");
        return false;
    }
    return ok;
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
    // Fail closed on a blank or relative output location: "" makes the output path
    // "/SettingsScreenshots/..." (drive root) and a relative one resolves against the
    // working directory, so screenshots and the report could land outside the intended
    // backup folder.
    if (m_output_location.trimmed().isEmpty() || !QDir::isAbsolutePath(m_output_location)) {
        ExecutionResult result;
        result.success = false;
        result.message = QStringLiteral("Screenshot output location is invalid");
        result.log = QStringLiteral("Refusing to capture to a blank or relative path");
        finishWithResult(result, ActionStatus::Failed);
        return;
    }
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_EMIT executionProgress("Detecting monitor configuration...", progress::kStep3);
    int monitor_count = detectMonitorCount();

    Q_EMIT executionProgress("Preparing screenshot directory...", progress::kStep5);
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QDir output_dir(m_output_location + "/SettingsScreenshots/" + timestamp);
    if (!output_dir.mkpath(".")) {
        sak::logWarning("Failed to create screenshot output directory: {}",
                        output_dir.absolutePath().toStdString());
    }

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

        int progress = progress::kStep5 + (processed * progress::kStep90) / settings_pages.size();
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

    Q_EMIT executionProgress("Generating report...", progress::kStep95);
    const bool report_written =
        generateReport(output_dir.absolutePath(), timestamp, monitor_count, capture);
    Q_EMIT executionProgress("Screenshots complete", progress::kComplete);

    const CaptureContext context{static_cast<int>(settings_pages.size()),
                                 monitor_count,
                                 timestamp,
                                 start_time,
                                 report_written};
    buildExecutionResult(capture, output_dir, context);
}

void ScreenshotSettingsAction::buildExecutionResult(const CaptureResult& capture,
                                                    const QDir& output_dir,
                                                    const CaptureContext& context) {
    qint64 duration_ms = context.start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = capture.screenshots_taken;
    result.output_path = output_dir.absolutePath();

    QString structured_log;
    structured_log += QString("MONITORS_DETECTED:%1\n").arg(context.monitor_count);
    structured_log += QString("SUCCESSFUL_CAPTURES:%1\n").arg(capture.captured_pages.size());
    structured_log += QString("FAILED_CAPTURES:%1\n").arg(capture.failed_attempts);
    structured_log += QString("TOTAL_PAGES:%1\n").arg(context.total_pages);
    structured_log += QString("SUCCESS_RATE:%1%\n")
                          .arg(capture.captured_pages.size() * kPercentMax / context.total_pages);
    const QString report_path =
        output_dir.filePath(QString("Screenshot_Report_%1.txt").arg(context.timestamp));
    structured_log += reportPathLine(context.report_written, report_path);

    // Fail closed: only a full capture (every page) with a durably written report
    // is a success. A partial capture or an unwritten report is surfaced as a
    // failure rather than passed off as a complete settings backup.
    const bool full_success = capture.screenshots_taken > 0 && capture.failed_attempts == 0 &&
                              context.report_written;
    result.success = full_success;

    if (full_success) {
        result.message = QString("Captured %1/%2 settings pages (%3 monitors detected)")
                             .arg(capture.captured_pages.size())
                             .arg(context.total_pages)
                             .arg(context.monitor_count);
        result.log = structured_log + QString("\nSaved to: %1").arg(output_dir.absolutePath());
    } else if (capture.screenshots_taken == 0) {
        result.message = "Failed to capture any screenshots";
        result.log = structured_log + "\nCheck display permissions and Settings app availability";
    } else {
        result.message = QString("Incomplete: captured %1/%2 pages, %3 failed%4")
                             .arg(capture.captured_pages.size())
                             .arg(context.total_pages)
                             .arg(capture.failed_attempts)
                             .arg(context.report_written ? QString()
                                                         : QStringLiteral(", report not written"));
        result.log = structured_log + QString("\nSaved to: %1").arg(output_dir.absolutePath());
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

// --- Private Helpers ------------------------------------------------------------

QMap<QString, QString> ScreenshotSettingsAction::buildSettingsPageMap() {
    return {{"about", "System_About"},
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
            {"gaming", "Gaming_Settings"}};
}

void ScreenshotSettingsAction::closeSettingsApp() {
    // System32-qualify taskkill.exe: a bare name resolves through the CreateProcess
    // search order (current directory and PATH ahead of System32), so a planted
    // taskkill.exe would run instead. Fail closed if the path cannot be resolved.
    const QString taskkill = sak::system32Path(QStringLiteral("taskkill.exe"));
    if (taskkill.isEmpty()) {
        Q_EMIT logMessage(
            QStringLiteral("Settings close skipped: cannot resolve System32 taskkill.exe path"));
        return;
    }
    ProcessResult close_proc = runProcess(taskkill,
                                          QStringList() << "/IM" << "SystemSettings.exe" << "/F",
                                          sak::kTimeoutProcessMediumMs);
    if (!close_proc.succeeded()) {
        Q_EMIT logMessage("Settings close warning: " + close_proc.std_err.trimmed());
    }
}

bool ScreenshotSettingsAction::captureSettingsWindow(const QDir& output_dir,
                                                     const QString& page_name,
                                                     const QString& timestamp) {
    // Find the Settings window by enumerating top-level windows
    HWND settings_hwnd = nullptr;
    EnumWindows(
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!proc) {
                return TRUE;
            }
            wchar_t exe_path[MAX_PATH] = {};
            DWORD path_size = MAX_PATH;
            bool found = false;
            if (QueryFullProcessImageNameW(proc, 0, exe_path, &path_size)) {
                // Require an EXACT executable-name match, not a substring anywhere in the
                // path: "SystemSettings" occurs in any folder named that, so a decoy exe
                // under such a path (or named SystemSettingsX.exe) would otherwise be
                // trusted as the real Settings window and captured as evidence.
                const QString exe_name = QFileInfo(QString::fromWCharArray(exe_path)).fileName();
                found = exe_name.compare(QStringLiteral("SystemSettings.exe"),
                                         Qt::CaseInsensitive) == 0;
            }
            CloseHandle(proc);
            if (found) {
                *reinterpret_cast<HWND*>(lparam) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&settings_hwnd));

    if (!settings_hwnd) {
        // Refuse to capture an arbitrary foreground window -- the caller
        // asked for Settings, not "whatever happens to be on top".
        sak::logWarning("ScreenshotSettingsAction: Settings window not found for page {}",
                        page_name.toStdString());
        return false;
    }

    const QString filepath = output_dir.filePath(QString("%1_%2.png").arg(page_name, timestamp));
    // grabWindow must run on the GUI thread; execute() runs on the worker thread, so marshal it.
    return captureWindowToPng(reinterpret_cast<WId>(settings_hwnd), filepath);
}

bool ScreenshotSettingsAction::captureSettingsPage(const QString& ms_uri,
                                                   const QString& page_name,
                                                   const QString& output_dir_path,
                                                   const QString& timestamp) {
    QDir output_dir(output_dir_path);
    for (int attempt = kSettingsCaptureFirstAttempt; attempt <= kSettingsCaptureMaxAttempts;
         ++attempt) {
        // explorer.exe by absolute Windows-directory path (it is NOT a System32 binary):
        // a shell-open resolves through the same CreateProcess search order as any other
        // launch, so a bare name lets a planted explorer.exe run. Refuse the whole capture
        // attempt rather than fall back to the bare name.
        if (!sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"),
                                           {QString("ms-settings:%1").arg(ms_uri)})) {
            return false;
        }

        const int wait_time = kSettingsCaptureInitialWaitMs +
                              (attempt - kSettingsCaptureFirstAttempt) *
                                  kSettingsCaptureRetryWaitMs;
        QThread::msleep(wait_time);

        if (!isProcessRunning("SystemSettings.exe")) {
            closeSettingsApp();
            QThread::msleep(sak::kTimerRetryBaseMs);
            continue;
        }

        bool captured = captureSettingsWindow(output_dir, page_name, timestamp);
        closeSettingsApp();
        QThread::msleep(sak::kTimerRetryBaseMs);

        if (captured) {
            return true;
        }
    }
    return false;
}

QString ScreenshotSettingsAction::buildScreenshotReportText(int monitor_count,
                                                            const CaptureResult& capture,
                                                            const QString& output_dir_path,
                                                            const QString& timestamp) {
    Q_UNUSED(timestamp);
    const int total_pages = capture.captured_pages.size() + capture.failed_pages.size();

    QString text;
    QTextStream report(&text);
    report.setEncoding(QStringConverter::Utf8);

    report << "+==============================================================+\n";
    report << "|         WINDOWS SETTINGS SCREENSHOT REPORT                   |\n";
    report << "+==============================================================+\n";
    report << QString("| Timestamp:         %1                    |\n")
                  .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    report << QString("| Monitors Detected: %1                                       |\n")
                  .arg(monitor_count);
    report << QString("| Total Pages:       %1                                      |\n")
                  .arg(total_pages);
    report << QString("| Successful:        %1                                      |\n")
                  .arg(capture.captured_pages.size());
    report << QString("| Failed:            %1                                       |\n")
                  .arg(capture.failed_attempts);
    report << "+==============================================================+\n";
    report << "|                    CAPTURED PAGES                            |\n";
    report << "+==============================================================+\n";

    for (const QString& page : capture.captured_pages) {
        report << QString("| [x] %1").arg(page).leftJustified(kScreenshotReportFieldWidth, ' ')
               << "|\n";
    }

    if (!capture.failed_pages.isEmpty()) {
        report << "+==============================================================+\n";
        report << "|                     FAILED PAGES                             |\n";
        report << "+==============================================================+\n";
        for (const QString& page : capture.failed_pages) {
            report << QString("| [ ] %1").arg(page).leftJustified(kScreenshotReportFieldWidth, ' ')
                   << "|\n";
        }
    }

    report << "+==============================================================+\n";
    report << QString("| Output Location: %1")
                  .arg(QDir(output_dir_path).absolutePath())
                  .leftJustified(kScreenshotReportFieldWidth, ' ')
           << "|\n";
    report << "+==============================================================+\n";
    report.flush();
    return text;
}

QString ScreenshotSettingsAction::reportPathLine(bool report_written, const QString& report_path) {
    // Only advertise the report path when the file actually exists; otherwise
    // surface the write failure instead of pointing at a file that was not saved.
    if (report_written) {
        return QString("REPORT_PATH:%1\n").arg(report_path);
    }
    return QStringLiteral("REPORT_WRITE_FAILED:1\n");
}

bool ScreenshotSettingsAction::generateReport(const QString& output_dir_path,
                                              const QString& timestamp,
                                              int monitor_count,
                                              const CaptureResult& capture) {
    QDir output_dir(output_dir_path);
    const QString report_path =
        output_dir.filePath(QString("Screenshot_Report_%1.txt").arg(timestamp));

    QSaveFile report_file(report_path);
    if (!report_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT logMessage("Failed to open screenshot report for writing: " + report_path);
        return false;
    }

    const QByteArray data =
        buildScreenshotReportText(monitor_count, capture, output_dir_path, timestamp).toUtf8();
    if (report_file.write(data) != data.size()) {
        report_file.cancelWriting();
        Q_EMIT logMessage("Incomplete write of screenshot report: " + report_path);
        return false;
    }
    if (!report_file.commit()) {
        Q_EMIT logMessage("Failed to commit screenshot report: " + report_path);
        return false;
    }
    return true;
}

// Helper method: Detect monitor count for multi-monitor support
int ScreenshotSettingsAction::detectMonitorCount() {
    // QGuiApplication::screens()/QScreen access is GUI-thread-affine, but execute() runs
    // on the quick-action worker thread. Marshal the query onto the GUI thread and block
    // until it finishes; fail closed (0) if there is no application object to marshal onto
    // or the invocation cannot be marshalled -- never touch QScreen off the GUI thread.
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        sak::logWarning("ScreenshotSettingsAction: no QCoreApplication to detect monitors");
        return 0;
    }
    if (QThread::currentThread() == app->thread()) {
        return countMonitorsOnGui();
    }
    int count = 0;
    const bool invoked = QMetaObject::invokeMethod(
        app, [&count]() { count = countMonitorsOnGui(); }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        sak::logWarning(
            "ScreenshotSettingsAction: failed to marshal monitor detection onto the GUI thread");
        return 0;
    }
    return count;
}

int ScreenshotSettingsAction::countMonitorsOnGui() {
    QList<QScreen*> screens = QGuiApplication::screens();
    int count = screens.size();

    sak::logDebug("Detected {} monitor(s)", count);

    // Log each monitor's properties
    for (int i = 0; i < count; i++) {
        QScreen* screen = screens[i];
        QRect geometry = screen->geometry();
        sak::logDebug("Monitor {}: {}x{} at {},{}",
                      i + 1,
                      geometry.width(),
                      geometry.height(),
                      geometry.x(),
                      geometry.y());
    }

    return count;
}

// Helper method: Check if a process is currently running
bool ScreenshotSettingsAction::isProcessRunning(const QString& process_name) {
    if (process_name.isEmpty()) {
        sak::logWarning("isProcessRunning: empty process_name");
        return false;
    }
    // System32-qualify tasklist.exe so a CWD/PATH-planted tasklist.exe cannot be run
    // in its place. Fail closed (treat as not running) if the path cannot be resolved.
    const QString tasklist = sak::system32Path(QStringLiteral("tasklist.exe"));
    if (tasklist.isEmpty()) {
        sak::logWarning("isProcessRunning: cannot resolve System32 tasklist.exe path");
        return false;
    }
    const auto result = sak::runProcess(tasklist,
                                        {QStringLiteral("/FI"),
                                         QStringLiteral("IMAGENAME eq %1").arg(process_name)},
                                        sak::kTimerServiceDelayMs);
    if (result.timed_out) {
        sak::logWarning("tasklist timed out checking for process: {}", process_name.toStdString());
        return false;
    }

    if (result.exit_code != 0) {
        sak::logDebug("tasklist returned exit code {} for process: {}",
                      result.exit_code,
                      process_name.toStdString());
        return false;
    }

    return result.std_out.contains(process_name, Qt::CaseInsensitive);
}

}  // namespace sak
