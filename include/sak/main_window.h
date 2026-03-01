// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QMainWindow>
#include <QTabWidget>
#include <QStatusBar>
#include <QProgressBar>
#include <QLabel>
#include <QMenuBar>
#include <QMap>
#include <QStringList>
#include <memory>

// Forward declarations for feature panels (in global namespace)
// -- none needed; all panels now in sak::

namespace sak {

// Forward declarations for feature panels
class UserMigrationPanel;
class OrganizerPanel;
class DuplicateFinderPanel;
class ImageFlasherPanel;
class AppInstallationPanel;
class QuickActionsPanel;
class NetworkTransferPanel;
class DiagnosticBenchmarkPanel;
class DetachableLogWindow;
class LogToggleSwitch;
class WifiManagerPanel;

/**
 * @brief Main application window for S.A.K. Utility
 * 
 * Provides a tabbed interface for all feature modules with integrated
 * progress tracking and status reporting.
 * 
 * Thread-Safety: GUI operations must be performed on the main Qt thread.
 * Use Qt signals/slots for cross-thread communication.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /**
     * @brief Construct main window
     * @param parent Optional parent widget
     */
    explicit MainWindow(QWidget* parent = nullptr);
    
    /**
     * @brief Destructor
     */
    ~MainWindow() override;

    // Disable copy and move (Qt QObject semantics)
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

public Q_SLOTS:
    /**
     * @brief Update status bar message
     * @param message Status message to display
     * @param timeout_ms Timeout in milliseconds (0 = permanent)
     */
    void updateStatus(const QString& message, int timeout_ms = 0);

    /**
     * @brief Update progress bar
     * @param current Current progress value
     * @param maximum Maximum progress value
     */
    void updateProgress(int current, int maximum);

    /**
     * @brief Show/hide progress bar
     * @param visible True to show, false to hide
     */
    void setProgressVisible(bool visible);

private Q_SLOTS:
    void onTabChanged(int index);

private:
    /**
     * @brief Initialize UI components
     */
    void setupUi();

    /**
     * @brief Create menu bar
     */
    void createMenuBar();

    /**
     * @brief Create status bar
     */
    void createStatusBar();

    /**
     * @brief Create feature panels
     */
    void createPanels();

    /**
     * @brief Create keyboard shortcuts for accessibility
     */
    void createKeyboardShortcuts();

    /**
     * @brief Load window state from settings
     */
    void loadWindowState();

    /**
     * @brief Save window state to settings
     */
    void saveWindowState();

protected:
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // Central widget - tab container
    QTabWidget* m_tab_widget{nullptr};

    // Feature panels
    std::unique_ptr<UserMigrationPanel> m_user_migration_panel;
    std::unique_ptr<OrganizerPanel> m_organizer_panel;
    std::unique_ptr<DuplicateFinderPanel> m_duplicate_finder_panel;
    std::unique_ptr<AppInstallationPanel> m_app_installation_panel;
    std::unique_ptr<ImageFlasherPanel> m_image_flasher_panel;
    std::unique_ptr<QuickActionsPanel> m_quick_actions_panel;
    std::unique_ptr<NetworkTransferPanel> m_network_transfer_panel;
    std::unique_ptr<DiagnosticBenchmarkPanel> m_diagnostic_benchmark_panel;
    std::unique_ptr<WifiManagerPanel> m_wifi_manager_panel;

    // Status bar components
    QLabel* m_status_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};

    // Shared log window
    DetachableLogWindow* m_logWindow{nullptr};

    // Per-panel log storage for tab-aware log switching
    QMap<int, QStringList> m_panelLogs;
};

} // namespace sak
