// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMenuBar>
#include <QProgressBar>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>

#include <memory>

class QFrame;
class QHBoxLayout;

// Forward declarations for feature panels (in global namespace)
// -- none needed; all panels now in sak::

namespace sak {

// Forward declarations for feature panels
class UserMigrationPanel;
class OrganizerPanel;
class ImageFlasherPanel;
class AppInstallationPanel;
class QuickActionsPanel;
class NetworkTransferPanel;
class DiagnosticBenchmarkPanel;
class DetachableLogWindow;
class LogToggleSwitch;
class WifiManagerPanel;
class AdvancedSearchPanel;
class AdvancedUninstallPanel;
class NetworkDiagnosticPanel;

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

    /// @brief Create all tool/feature panel tabs
    void createToolPanels();

    /// @brief Create simple standalone panel tabs
    void createSimplePanels();

    /// @brief Create the Application Management composite panel tab
    void createAppManagementPanel();

    /// @brief Create the Network Management composite panel tab
    void createNetworkManagementPanel();

    /// @brief Create the About information panel tab
    void createAboutPanel();

    /// @brief Create the Help & Support panel tab
    void createHelpPanel();

    /// @brief Build the Feature Request + Bug Report card row
    QHBoxLayout* createHelpRow_requestsAndBugs(QWidget* parent,
                                               const QString& cardStyle,
                                               const QString& titleStyle,
                                               const QString& descStyle,
                                               const QString& logoStyle);

    /// @brief Build the Wiki + Community card row
    QHBoxLayout* createHelpRow_wikiAndCommunity(QWidget* parent,
                                                const QString& cardStyle,
                                                const QString& titleStyle,
                                                const QString& descStyle,
                                                const QString& logoStyle);

    /// @brief Build the Discord community card
    QFrame* createCommunityCard(QWidget* parent,
                                const QString& cardStyle,
                                const QString& titleStyle,
                                const QString& descStyle,
                                const QString& logoStyle);

    /// @brief Load splash screen icon into the About panel header
    void loadAboutPanelIcon(QLabel* iconLabel);

    /// @brief Connect panel status and progress signals to the main window
    void connectPanelSignals();

    /// @brief Connect remaining panel status/progress signals
    void connectRemainingPanelSignals();

    /// @brief Connect panel log signals to the shared log window
    void connectPanelLogs();

    /// @brief Find the main tab index containing this panel widget
    [[nodiscard]] int findPanelTabIndex(QWidget* panel) const;

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

    /// @brief Append a log line and show it if the tab is active
    void appendLogIfActive(int tabIdx, const QString& formatted);

    /// @brief Hide the progress bar if it has reached maximum
    void hideProgressBarIfComplete();

    /// @brief Apply chevron icons to the tab bar scroll buttons
    void applyTabBarChevrons();

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
    std::unique_ptr<AppInstallationPanel> m_app_installation_panel;
    std::unique_ptr<ImageFlasherPanel> m_image_flasher_panel;
    std::unique_ptr<QuickActionsPanel> m_quick_actions_panel;
    std::unique_ptr<NetworkTransferPanel> m_network_transfer_panel;
    std::unique_ptr<DiagnosticBenchmarkPanel> m_diagnostic_benchmark_panel;
    std::unique_ptr<WifiManagerPanel> m_wifi_manager_panel;
    std::unique_ptr<AdvancedSearchPanel> m_advanced_search_panel;
    std::unique_ptr<AdvancedUninstallPanel> m_advanced_uninstall_panel;
    std::unique_ptr<NetworkDiagnosticPanel> m_network_diagnostic_panel;

    // Status bar components
    QLabel* m_status_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};

    // Shared log window
    DetachableLogWindow* m_logWindow{nullptr};

    // Per-panel log storage for tab-aware log switching
    QMap<int, QStringList> m_panelLogs;
};

}  // namespace sak
