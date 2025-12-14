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
class LicenseScannerPanel;
class ImageFlasherPanel;

namespace sak {
    class AppMigrationPanel;
    class QuickActionsPanel;
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
    void update_status(const QString& message, int timeout_ms = 0);

    /**
     * @brief Update progress bar
     * @param current Current progress value
     * @param maximum Maximum progress value
     */
    void update_progress(int current, int maximum);

    /**
     * @brief Show/hide progress bar
     * @param visible True to show, false to hide
     */
    void set_progress_visible(bool visible);

private Q_SLOTS:
    /**
     * @brief Handle About dialog
     */
    void on_about_clicked();

    /**
     * @brief Handle Exit action
     */
    void on_exit_clicked();

    /**
     * @brief Handle Settings dialog
     */
    void on_settings_clicked();

    /**
     * @brief Handle Undo action
     */
    void on_undo_clicked();

    /**
     * @brief Handle Redo action
     */
    void on_redo_clicked();

    /**
     * @brief Update undo/redo UI state
     * @param can_undo True if undo is available
     */
    void on_undo_available(bool can_undo);

    /**
     * @brief Update undo/redo UI state
     * @param can_redo True if redo is available
     */
    void on_redo_available(bool can_redo);

private:
    /**
     * @brief Initialize UI components
     */
    void setup_ui();

    /**
     * @brief Create menu bar
     */
    void create_menu_bar();

    /**
     * @brief Create toolbar
     */
    void create_toolbar();

    /**
     * @brief Create status bar
     */
    void create_status_bar();

    /**
     * @brief Setup undo/redo actions
     */
    void setup_undo_redo();

    /**
     * @brief Create feature panels
     */
    void create_panels();

    /**
     * @brief Load window state from settings
     */
    void load_window_state();

    /**
     * @brief Save window state to settings
     */
    void save_window_state();

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
    std::unique_ptr<LicenseScannerPanel> m_license_scanner_panel;
    std::unique_ptr<sak::AppMigrationPanel> m_app_migration_panel;
    std::unique_ptr<ImageFlasherPanel> m_image_flasher_panel;
    std::unique_ptr<sak::QuickActionsPanel> m_quick_actions_panel;

    // Status bar components
    QLabel* m_status_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};

    // Undo/redo actions
    QAction* m_undo_action{nullptr};
    QAction* m_redo_action{nullptr};
};
