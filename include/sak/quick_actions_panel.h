// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"

#include <QWidget>
#include <QGroupBox>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QTextEdit>
#include <QLineEdit>
#include <QCheckBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHash>
#include <memory>
#include <vector>

namespace sak {

class QuickActionController;

/**
 * @brief Quick Actions Panel - One-click technician operations
 * 
 * Provides instant access to common maintenance, backup, and troubleshooting
 * tasks without complex wizards. Actions are organized into collapsible
 * categories with real-time scanning to show size estimates before execution.
 * 
 * Features:
 * - System Optimization (cleanup, startup management, performance)
 * - Quick Backups (QuickBooks, browsers, email, game saves)
 * - Maintenance Tasks (disk health, updates, system repair)
 * - Troubleshooting (reports, bloatware detection, diagnostics)
 * - Emergency Recovery (rapid backup, restore points, license export)
 * 
 * Thread-Safety: All UI operations on main thread, actions run on workers
 */
class QuickActionsPanel : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent widget (typically MainWindow)
     */
    explicit QuickActionsPanel(QWidget* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~QuickActionsPanel() override;

    // Disable copy and move
    QuickActionsPanel(const QuickActionsPanel&) = delete;
    QuickActionsPanel& operator=(const QuickActionsPanel&) = delete;
    QuickActionsPanel(QuickActionsPanel&&) = delete;
    QuickActionsPanel& operator=(QuickActionsPanel&&) = delete;

Q_SIGNALS:
    /**
     * @brief Status message for main window status bar
     * @param message Status message
     * @param timeout_ms Timeout in milliseconds (0 = permanent)
     */
    void statusMessage(const QString& message, int timeout_ms);

    /**
     * @brief Progress update for main window
     * @param current Current progress value
     * @param maximum Maximum progress value
     */
    void progressUpdate(int current, int maximum);

private Q_SLOTS:
    /**
     * @brief Handle action button clicked
     * @param action Pointer to action that was clicked
     */
    void onActionClicked(QuickAction* action);

    /**
     * @brief Handle action scan completion
     * @param action Pointer to action that completed
     */
    void onActionScanComplete(QuickAction* action);

    /**
     * @brief Handle action execution progress
     * @param action Pointer to action
     * @param message Progress message
     * @param progress Progress value (0-100)
     */
    void onActionProgress(QuickAction* action, const QString& message, int progress);

    /**
     * @brief Handle action execution completion
     * @param action Pointer to action that completed
     */
    void onActionComplete(QuickAction* action);

    /**
     * @brief Handle action error
     * @param action Pointer to action that failed
     * @param error_message Error description
     */
    void onActionError(QuickAction* action, const QString& error_message);

    /**
     * @brief Handle browse button for backup location
     */
    void onBrowseBackupLocation();

    /**
     * @brief Handle backup location changed
     */
    void onBackupLocationChanged();

    /**
     * @brief Handle open backup folder button
     */
    void onOpenBackupFolder();

    /**
     * @brief Handle view log button
     */
    void onViewLog();

    /**
     * @brief Handle settings checkbox changes
     */
    void onSettingChanged();

    /**
     * @brief Show settings dialog
     */
    void showSettingsDialog();

    /**
     * @brief Refresh all action scans
     */
    void refreshAllScans();

private:
    /**
     * @brief Format bytes for display
     * @param bytes Number of bytes to format
     * @return Formatted string (e.g., "2.3 GB")
     */
    static QString formatBytes(qint64 bytes);

    /**
     * @brief Format duration for display
     * @param seconds Duration in seconds
     * @return Formatted string (e.g., "2m 30s")
     */
    static QString formatDuration(qint64 seconds);

    /**
     * @brief Initialize UI components
     */
    void setupUi();

    /**
     * @brief Create all action instances
     */
    void createActions();

    /**
     * @brief Create category sections
     */
    void createCategorySections();

    /**
     * @brief Create action button for UI
     * @param action Action instance
     * @return Button widget
     */
    QPushButton* createActionButton(QuickAction* action);

    /**
     * @brief Update button with scan results
     * @param action Action instance
     */
    void updateActionButton(QuickAction* action);

    /**
     * @brief Create settings section
     */
    QGroupBox* createSettingsSection();

    /**
     * @brief Create status section
     */
    QGroupBox* createStatusSection();

    /**
     * @brief Setup signal connections
     */
    void setupConnections();

    /**
     * @brief Load settings from config
     */
    void loadSettings();

    /**
     * @brief Save settings to config
     */
    void saveSettings();

    /**
     * @brief Append message to log
     * @param message Log message
     */
    void appendLog(const QString& message);

    /**
     * @brief Update status display with latest action result
     */
    void updateStatusDisplay();

    /**
     * @brief Get category display name
     * @param category Action category
     * @return User-friendly category name
     */
    static QString getCategoryName(QuickAction::ActionCategory category);

    /**
     * @brief Get category icon
     * @param category Action category
     * @return Category icon
     */
    static QIcon getCategoryIcon(QuickAction::ActionCategory category);

    // Controller
    QuickActionController* m_controller{nullptr};

    // Actions (owned by controller)
    QHash<QString, QuickAction*> m_actions;

    // UI Components - Category sections
    QHash<QuickAction::ActionCategory, QGroupBox*> m_category_sections;
    QHash<QuickAction*, QPushButton*> m_action_buttons;

    // UI Components - Settings
    QLineEdit* m_backup_location_edit{nullptr};
    QPushButton* m_browse_button{nullptr};
    QCheckBox* m_confirm_checkbox{nullptr};
    QCheckBox* m_notifications_checkbox{nullptr};
    QCheckBox* m_logging_checkbox{nullptr};
    QCheckBox* m_compression_checkbox{nullptr};

    // UI Components - Status
    QVBoxLayout* m_actions_layout{nullptr};
    QLabel* m_action_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};
    QLabel* m_status_label{nullptr};
    QLabel* m_location_label{nullptr};
    QLabel* m_duration_label{nullptr};
    QLabel* m_bytes_label{nullptr};
    QPushButton* m_open_folder_button{nullptr};
    QPushButton* m_view_log_button{nullptr};

    // UI Components - Log
    QTextEdit* m_log_viewer{nullptr};

    // State
    QString m_backup_location;
    bool m_confirm_before_execute{true};
    bool m_show_notifications{true};
    bool m_enable_logging{true};
    bool m_compress_backups{true};
    QuickAction* m_current_action{nullptr};
    QDateTime m_last_action_time;
    QString m_last_output_path;
};

} // namespace sak
