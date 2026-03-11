// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QDir>
#include <QString>

namespace sak {

/**
 * @brief Screenshot Settings Action
 *
 * Captures screenshots of all Windows Settings pages for documentation.
 */
class ScreenshotSettingsAction : public QuickAction {
    Q_OBJECT

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
    bool isProcessRunning(const QString& process_name);
    void openSettingsAndCapture(const QString& page, const QString& filename);
    void captureScreen(const QString& filename);
    void openSettings(const QString& page);
    void captureScreen();
    void waitForWindow(int ms);

    static QMap<QString, QString> buildSettingsPageMap();
    bool captureSettingsPage(const QString& ms_uri,
                             const QString& page_name,
                             const QString& output_dir_path,
                             const QString& timestamp);
    /// @brief Kill SystemSettings.exe and log any warning
    void closeSettingsApp();
    /// @brief Capture screenshots from all monitors into output_dir
    bool captureAllScreens(const QDir& output_dir,
                           const QString& page_name,
                           const QString& timestamp);
    void generateReport(const QString& output_dir_path,
                        const QString& timestamp,
                        int monitor_count,
                        const CaptureResult& capture) const;

    /// @brief Context needed to assemble the final execution result
    struct CaptureContext {
        int total_pages{0};
        int monitor_count{0};
        QString timestamp;
        QDateTime start_time;
    };

    /// @brief Assemble structured log and ExecutionResult from capture outcome
    void buildExecutionResult(const CaptureResult& capture,
                              const QDir& output_dir,
                              const CaptureContext& context);
};

}  // namespace sak
