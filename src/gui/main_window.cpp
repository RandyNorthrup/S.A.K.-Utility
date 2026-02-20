#include "sak/main_window.h"
#include "sak/version.h"
#include "sak/backup_panel.h"
#include "sak/organizer_panel.h"
#include "sak/duplicate_finder_panel.h"
#include "sak/app_migration_panel.h"
#include "sak/image_flasher_panel.h"
#include "sak/quick_actions_panel.h"
#include "sak/network_transfer_panel.h"
#include "sak/config_manager.h"
#include "gui/settings_dialog.h"

#include <QAction>
#include <QCloseEvent>
#include <QMessageBox>

using sak::AppMigrationPanel;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setup_ui();
    load_window_state();
}

MainWindow::~MainWindow()
{
    save_window_state();
}

void MainWindow::setup_ui()
{
    setWindowTitle("S.A.K. Utility - Swiss Army Knife Utility");
    setMinimumSize(1024, 768);
    
    // Create central tab widget
    m_tab_widget = new QTabWidget(this);
    m_tab_widget->setTabPosition(QTabWidget::North);
    m_tab_widget->setDocumentMode(true);
    setCentralWidget(m_tab_widget);
    
    // Create UI elements
    create_menu_bar();
    create_toolbar();
    create_status_bar();
    create_panels();
    
    update_status("Ready", 0);
}

void MainWindow::create_menu_bar()
{
    auto* file_menu = menuBar()->addMenu("&File");
    
    auto* exit_action = new QAction("E&xit", this);
    exit_action->setShortcut(QKeySequence::Quit);
    exit_action->setStatusTip("Exit the application");
    connect(exit_action, &QAction::triggered, this, &MainWindow::on_exit_clicked);
    file_menu->addAction(exit_action);
    
    auto* edit_menu = menuBar()->addMenu("&Edit");
    
    auto* settings_action = new QAction("&Settings", this);
    settings_action->setShortcut(QKeySequence::Preferences);
    settings_action->setStatusTip("Open settings dialog");
    connect(settings_action, &QAction::triggered, this, &MainWindow::on_settings_clicked);
    edit_menu->addAction(settings_action);
    
    auto* help_menu = menuBar()->addMenu("&Help");
    
    auto* about_action = new QAction("&About", this);
    about_action->setStatusTip("About S.A.K. Utility");
    connect(about_action, &QAction::triggered, this, &MainWindow::on_about_clicked);
    help_menu->addAction(about_action);
}

void MainWindow::create_toolbar()
{
    auto* toolbar = addToolBar("Main Toolbar");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    
}

void MainWindow::create_status_bar()
{
    // Status bar disabled - panels have their own status bars
    statusBar()->hide();
}

void MainWindow::create_panels()
{
    // Create Quick Actions panel (first tab)
    m_quick_actions_panel = std::make_unique<sak::QuickActionsPanel>(this);
    m_tab_widget->addTab(m_quick_actions_panel.get(), "Quick Actions");
    
    // Create User Migration panel
    m_backup_panel = std::make_unique<BackupPanel>(this);
    m_tab_widget->addTab(m_backup_panel.get(), "User Migration");
    
    // Create Organizer panel
    m_organizer_panel = std::make_unique<OrganizerPanel>(this);
    m_tab_widget->addTab(m_organizer_panel.get(), "Directory Organizer");
    
    // Create Duplicate Finder panel
    m_duplicate_finder_panel = std::make_unique<DuplicateFinderPanel>(this);
    m_tab_widget->addTab(m_duplicate_finder_panel.get(), "Duplicate Finder");
    
    // Create App Migration panel
    m_app_migration_panel = std::make_unique<AppMigrationPanel>(this);
    m_tab_widget->addTab(m_app_migration_panel.get(), "App Migration");

    // Create Network Transfer panel
    if (sak::ConfigManager::instance().getNetworkTransferEnabled()) {
        m_network_transfer_panel = std::make_unique<sak::NetworkTransferPanel>(this);
        m_tab_widget->addTab(m_network_transfer_panel.get(), "Network Transfer");
    }
    
    // Create Image Flasher panel
    m_image_flasher_panel = std::make_unique<ImageFlasherPanel>(this);
    m_tab_widget->addTab(m_image_flasher_panel.get(), "Image Flasher");
    
    // Connect panel signals to main window status bar
    connect(m_quick_actions_panel.get(), &sak::QuickActionsPanel::status_message,
            this, [this](const QString& msg) { update_status(msg, 5000); });
    connect(m_quick_actions_panel.get(), &sak::QuickActionsPanel::progress_update,
            this, &MainWindow::update_progress);
    
    connect(m_backup_panel.get(), &BackupPanel::status_message,
            this, [this](const QString& msg) { update_status(msg, 5000); });
    // User Migration panel has its own progress bar, no progress_updated signal needed
    
    connect(m_app_migration_panel.get(), &AppMigrationPanel::status_message,
            this, [this](const QString& msg) { update_status(msg, 5000); });
    connect(m_app_migration_panel.get(), &AppMigrationPanel::progress_updated,
            this, &MainWindow::update_progress);

            if (m_network_transfer_panel) {
            connect(m_network_transfer_panel.get(), &sak::NetworkTransferPanel::status_message,
                this, [this](const QString& msg) { update_status(msg, 5000); });
            connect(m_network_transfer_panel.get(), &sak::NetworkTransferPanel::progress_update,
                this, &MainWindow::update_progress);
            }
    
}

void MainWindow::update_status(const QString& message, int timeout_ms)
{
    if (m_status_label) {
        if (timeout_ms > 0) {
            statusBar()->showMessage(message, timeout_ms);
        } else {
            m_status_label->setText(message);
        }
    }
}

void MainWindow::update_progress(int current, int maximum)
{
    if (m_progress_bar) {
        m_progress_bar->setMaximum(maximum);
        m_progress_bar->setValue(current);
    }
}

void MainWindow::set_progress_visible(bool visible)
{
    if (m_progress_bar) {
        m_progress_bar->setVisible(visible);
    }
}

void MainWindow::on_about_clicked()
{
    QMessageBox::about(this, "About S.A.K. Utility",
        QString("<h2>S.A.K. Utility v%1</h2>"
        "<p>Swiss Army Knife Utility - PC Technician's Toolkit</p>"
        "<p>Copyright © 2025 Randy Northrup</p>"
        "<p>Built with Qt 6.5.3 and C++23</p>"
        "<p>Features:</p>"
        "<ul>"
        "<li>User Profile Migration & Restore</li>"
        "<li>Application Migration</li>"
        "<li>Directory Organization</li>"
        "<li>Duplicate File Detection</li>"
        "<li>Image Flasher & ISO Downloads</li>"
        "</ul>").arg(sak::get_version_short()));
}

void MainWindow::on_exit_clicked()
{
    close();
}

void MainWindow::on_settings_clicked()
{
    sak::SettingsDialog dialog(this);
    dialog.exec();
}

void MainWindow::load_window_state()
{
    auto& config = sak::ConfigManager::instance();
    
    if (config.getRestoreWindowGeometry()) {
        restoreGeometry(config.getWindowGeometry());
        restoreState(config.getWindowState());
    }
    
    // Always start on Quick Actions tab (index 0)
    m_tab_widget->setCurrentIndex(0);
}

void MainWindow::save_window_state()
{
    auto& config = sak::ConfigManager::instance();
    config.setWindowGeometry(saveGeometry());
    config.setWindowState(saveState());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    event->accept();
}
