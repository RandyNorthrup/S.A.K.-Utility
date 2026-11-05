// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QString>
#include <qwindowdefs.h>              // WId

class ScreenshotSettingsActionTests;  // unit-test friend (global namespace)

namespace sak {

/**
 * @brief Screenshot Settings Action
 *
 * Captures screenshots of configured key Windows Settings pages for documentation.
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

    void execute() override;

private:
    /// The Settings window that was open before this run (the user's), or 0. Never closed.
    WId m_settings_window_before_launch{0};

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
    /// @brief True when a picture is one colour throughout, which is what copying a window drawn
    ///        by the compositor gives. Such a picture is a failed capture, never a saved one.
    [[nodiscard]] static bool pictureIsBlank(const QImage& image);
    /// @brief True when @p now shows a page that has finished drawing: not one colour, the same
    ///        as @p moment_before, and not @p previous_page, which the reused window showed last.
    [[nodiscard]] static bool pageIsDrawn(const QImage& now,
                                          const QImage& moment_before,
                                          const QImage& previous_page);
    /// @brief The picture @p window renders of itself, taken on the GUI thread
    ///        (BlockingQueuedConnection) so a worker thread can capture safely; null (fails
    ///        closed) if there is no QCoreApplication or the invocation cannot be marshalled.
    static QImage renderOnGui(WId window);
    /// @brief Brings @p window to the front and waits, bounded and cancellable, for it to draw a
    ///        page other than the last one saved. Null when it does not.
    QImage waitForDrawnPage(WId window);
    /// The last page saved, which the reused Settings window shows until it draws the next.
    QImage m_previous_page;

    static QMap<QString, QString> buildSettingsPageMap();
    bool captureSettingsPage(const QString& ms_uri,
                             const QString& page_name,
                             const QString& output_dir_path,
                             const QString& timestamp);
    /// @brief The visible SystemSettings.exe top-level window, found by EnumWindows, or 0.
    static WId findSettingsWindow();
    /// @brief Poll (bounded, cancellable) until a Settings window exists and answers messages.
    WId findRespondingSettingsWindow();
    /// @brief Pure: the window this run may close -- @p settings_window, unless it is the one
    ///        that was already open before the run (@p window_before_launch), or none.
    static quintptr windowToClose(quintptr window_before_launch, quintptr settings_window);
    /// @brief WM_CLOSE to the Settings window this run opened (never the user's), then wait.
    void closeSettingsWindowWeOpened();
    /// @brief Capture only the Settings window @p settings_window (not entire screen)
    bool captureSettingsWindow(WId settings_window,
                               const QDir& output_dir,
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

    /// Ends the run as failed when the output folder is blank or relative; true when it did.
    bool rejectInvalidOutputFolder();

    /// Creates the run's capture folder; the error to report when it cannot (empty on success).
    [[nodiscard]] static QString createCaptureFolder(const QDir& output_dir);

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
