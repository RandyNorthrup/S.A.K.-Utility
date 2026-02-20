#pragma once

#include <QMainWindow>
#include <QTabWidget>
#include <QStatusBar>
#include <QProgressBar>
#include <QLabel>
#include <QMenuBar>
#include <QToolBar>
#include <memory>

// Forward declarations for feature panels
class BackupPanel;
class OrganizerPanel;
class DuplicateFinderPanel;
class ImageFlasherPanel;

namespace sak {
    class AppMigrationPanel;
    class QuickActionsPanel;
    class NetworkTransferPanel;
}

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
    /**
     * @brief Handle About dialog
     */
    void onAboutClicked();

    /**
     * @brief Handle Exit action
     */
    void onExitClicked();

    /**
     * @brief Handle Settings dialog
     */
    void onSettingsClicked();

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
     * @brief Create toolbar
     */
    void createToolbar();

    /**
     * @brief Create status bar
     */
    void createStatusBar();

    /**
     * @brief Create feature panels
     */
    void createPanels();

    /**
     * @brief Load window state from settings
     */
    void loadWindowState();

    /**
     * @brief Save window state to settings
     */
    void saveWindowState();

protected:
    /**
     * @brief Handle window close event
     * @param event Close event
     */
    void closeEvent(QCloseEvent* event) override;

private:
    // Central widget - tab container
    QTabWidget* m_tab_widget{nullptr};

    // Feature panels
    std::unique_ptr<BackupPanel> m_backup_panel;
    std::unique_ptr<OrganizerPanel> m_organizer_panel;
    std::unique_ptr<DuplicateFinderPanel> m_duplicate_finder_panel;
    std::unique_ptr<sak::AppMigrationPanel> m_app_migration_panel;
    std::unique_ptr<ImageFlasherPanel> m_image_flasher_panel;
    std::unique_ptr<sak::QuickActionsPanel> m_quick_actions_panel;
    std::unique_ptr<sak::NetworkTransferPanel> m_network_transfer_panel;

    // Status bar components
    QLabel* m_status_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};
};
