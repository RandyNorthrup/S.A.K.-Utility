// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
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

    // Helper methods for enterprise screenshot functionality
    int detectMonitorCount();
    bool isProcessRunning(const QString& process_name);
    void openSettingsAndCapture(const QString& page, const QString& filename);
    void captureScreen(const QString& filename);
    void openSettings(const QString& page);
    void captureScreen();
    void waitForWindow(int ms);
};

} // namespace sak

