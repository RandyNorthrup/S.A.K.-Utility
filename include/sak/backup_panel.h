// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QTextEdit>
#include <QTableView>
#include <QStandardItemModel>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <memory>
#include <vector>

namespace sak {
class UserDataManager;
class LogToggleSwitch;
}

/**
 * @brief Windows User Migration Panel
 * 
 * Provides comprehensive user profile migration and restore functionality:
 * - User-friendly backup wizard with automatic profile scanning
 * - Intelligent folder selection (Documents, Desktop, Pictures, etc.)
 * - Permission handling and elevation when needed
 * - Restore wizard with user mapping and conflict resolution
 * - Detailed operation logging and progress tracking
 * 
 * Similar to Application Migration panel but for user data.
 */
class BackupPanel : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent widget
     */
    explicit BackupPanel(QWidget* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~BackupPanel() override;

    // Disable copy and move
    BackupPanel(const BackupPanel&) = delete;
    BackupPanel& operator=(const BackupPanel&) = delete;
    BackupPanel(BackupPanel&&) = delete;
    BackupPanel& operator=(BackupPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    sak::LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Main actions
    void onBackupSelected();
    void onRestoreBackup();
    void onSettingsClicked();

private:
    void setupUi();
    void setupConnections();
    void appendLog(const QString& message);

    // UI Components
    QPushButton* m_backupButton{nullptr};
    QPushButton* m_restoreButton{nullptr};
    sak::LogToggleSwitch* m_logToggle{nullptr};
    
    // Data
    std::shared_ptr<sak::UserDataManager> m_dataManager;
};
