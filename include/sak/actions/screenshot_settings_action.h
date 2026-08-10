// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QDir>
#include <QString>
#include <qwindowdefs.h>              // WId

class ScreenshotSettingsActionTests;  // unit-test friend (global namespace)

namespace sak {

/**
 * @brief Screenshot Settings Action
 *
 * Captures screenshots of all Windows Settings pages for documentation.
 */
class ScreenshotSettingsAction : public QuickAction {
    Q_OBJECT

    friend class ::ScreenshotSettingsActionTests;

public:
    explicit ScreenshotSettingsAction(const QString& output_location, QObject* parent = nullptr);

    QString name() const override { return "Screenshot Settings"; }
    QString description() const override { return "Capture screenshots of Windows Settings"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_output_location;
    int m_screenshots_taken{0};

    /// @brief Result of capturing all settings pages
    struct CaptureResult {
        int screenshots_taken{0};
        int failed_attempts{0};
        QStringList captured_pages;
        QStringList failed_pages;
    };

    int detectMonitorCount();
    /// @brief Count and log the connected monitors. MUST run on the GUI thread
    ///        (QGuiApplication::screens()/QScreen access is GUI-thread-affine);
    ///        detectMonitorCount() marshals it there from the worker thread.
    static int countMonitorsOnGui();
    bool isProcessRunning(const QString& process_name);
    /// @brief Grab @p window (0 = whole primary screen) and save it as PNG to @p filepath.
    ///        MUST run on the GUI thread (QScreen::grabWindow has GUI-thread affinity). Returns
    ///        false on no primary screen, a null grab, or a failed save.
    static bool grabAndSaveOnGui(WId window, const QString& filepath);
    /// @brief Marshal grabAndSaveOnGui onto the GUI thread (BlockingQueuedConnection) so a worker
    ///        thread can capture safely; fails closed (returns false) if there is no
    ///        QCoreApplication or the invocation cannot be marshalled.
    static bool captureWindowToPng(WId window, const QString& filepath);

    static QMap<QString, QString> buildSettingsPageMap();
    bool captureSettingsPage(const QString& ms_uri,
                             const QString& page_name,
                             const QString& output_dir_path,
                             const QString& timestamp);
    /// @brief Kill SystemSettings.exe and log any warning
    void closeSettingsApp();
    /// @brief Capture only the Settings window (not entire screen)
    bool captureSettingsWindow(const QDir& output_dir,
                               const QString& page_name,
                               const QString& timestamp);
    /// @brief Write the screenshot report to disk.
    /// @return true only if the report was fully written and committed; a write
    /// failure is surfaced (the report path is not advertised as if it exists).
    bool generateReport(const QString& output_dir_path,
                        const QString& timestamp,
                        int monitor_count,
                        const CaptureResult& capture);

    /// @brief Build the full screenshot report text (pure; no I/O).
    static QString buildScreenshotReportText(int monitor_count,
                                             const CaptureResult& capture,
                                             const QString& output_dir_path,
                                             const QString& timestamp);

    /// @brief Structured-log line for the report: the real path only when the
    /// report was actually written, otherwise a write-failure marker.
    static QString reportPathLine(bool report_written, const QString& report_path);

    /// @brief Context needed to assemble the final execution result
    struct CaptureContext {
        int total_pages{0};
        int monitor_count{0};
        QString timestamp;
        QDateTime start_time;
        bool report_written{false};
    };

    /// @brief Assemble structured log and ExecutionResult from capture outcome
    void buildExecutionResult(const CaptureResult& capture,
                              const QDir& output_dir,
                              const CaptureContext& context);
};

}  // namespace sak
