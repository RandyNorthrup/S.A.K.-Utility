// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QCheckBox>
#include <QEvent>
#include <QFrame>
#include <QGroupBox>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <vector>

namespace sak {

class QuickActionController;
class DetachableLogWindow;
class LogToggleSwitch;

/**
 * @brief Quick Actions Panel - One-click technician operations
 *
 * Provides instant access to common maintenance, backup, and troubleshooting
 * tasks without complex wizards. Actions are organized into collapsible
 * categories with real-time scanning to show size estimates before execution.
 *
 * Features:
 * - System Optimization (cleanup, startup management, performance)
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

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

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
     * @brief Handle open backup folder button
     */
    void onOpenBackupFolder();

    /**
     * @brief Handle view log button
     */
    // cppcheck-suppress functionStatic
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
    void setupUi_statusSection(QVBoxLayout* main_layout);
    void setupUi_bottomRow(QVBoxLayout* main_layout);

    /**
     * @brief Create all action instances
     */
    void createActions();

    /**
     * @brief Create category sections
     */
    void createCategorySections();

    /**
     * @brief Create a clickable category card widget
     * @param category Action category enum
     * @param title Category title text
     * @param description Category description text
     * @param actionCount Number of actions in this category
     * @return Card frame widget
     */
    QFrame* createCategoryCard(QuickAction::ActionCategory category,
                               const QString& title,
                               const QString& description,
                               int actionCount);

    /**
     * @brief Show a modal dialog with the category's action library
     * @param category Action category enum
     * @param title Category title text
     */
    void showCategoryDialog(QuickAction::ActionCategory category, const QString& title);

    /**
     * @brief Update an action card status label
     * @param label Status label to update
     * @param action Action whose status to reflect
     */
    void updateActionCardStatus(QLabel* label, QuickAction* action);

    /**
     * @brief Event filter for card click handling
     */
    bool eventFilter(QObject* obj, QEvent* event) override;

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

    // Controller
    QuickActionController* m_controller{nullptr};

    // Actions (owned by controller)
    QHash<QString, QuickAction*> m_actions;

    // UI Components - Category sections
    QHash<QuickAction::ActionCategory, QFrame*> m_category_sections;
    QHash<QuickAction*, QPushButton*> m_action_buttons;

    // UI Components - Settings
    QCheckBox* m_confirm_checkbox{nullptr};
    QCheckBox* m_notifications_checkbox{nullptr};
    QCheckBox* m_logging_checkbox{nullptr};
    QCheckBox* m_compression_checkbox{nullptr};

    // UI Components - Status
    QVBoxLayout* m_actions_layout{nullptr};
    QLabel* m_action_label{nullptr};
    QLabel* m_status_label{nullptr};
    QLabel* m_location_label{nullptr};
    QLabel* m_duration_label{nullptr};
    QLabel* m_bytes_label{nullptr};
    QPushButton* m_open_folder_button{nullptr};
    QPushButton* m_view_log_button{nullptr};

    // UI Components - Log
    LogToggleSwitch* m_logToggle{nullptr};

    // State
    bool m_confirm_before_execute{true};
    bool m_show_notifications{true};
    bool m_enable_logging{true};
    bool m_compress_backups{true};
    QuickAction* m_current_action{nullptr};
    QDateTime m_last_action_time;
    QString m_last_output_path;
};

}  // namespace sak
