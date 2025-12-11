#include "sak/main_window.h"
#include "sak/version.h"
#include "sak/backup_panel.h"
#include "sak/organizer_panel.h"
#include "sak/duplicate_finder_panel.h"
#include "sak/license_scanner_panel.h"
#include "sak/app_migration_panel.h"
#include "gui/settings_dialog.h"
#include "gui/undo_manager.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QMessageBox>
#include <QSettings>
#include <QVBoxLayout>

using sak::AppMigrationPanel;

// Note: Logger access will be added in Phase 3 with proper async integration

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setup_ui();
    load_window_state();
    
    // Logger will be wired in Phase 3
}

MainWindow::~MainWindow()
{
    save_window_state();
    // Logger will be wired in Phase 3
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
    setup_undo_redo();
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
    
    // Undo/Redo actions (will be set up in setup_undo_redo)
    m_undo_action = new QAction("&Undo", this);
    m_undo_action->setShortcut(QKeySequence::Undo);
    m_undo_action->setStatusTip("Undo last operation");
    m_undo_action->setEnabled(false);
    connect(m_undo_action, &QAction::triggered, this, &MainWindow::on_undo_clicked);
    edit_menu->addAction(m_undo_action);
    
    m_redo_action = new QAction("&Redo", this);
    m_redo_action->setShortcut(QKeySequence::Redo);
    m_redo_action->setStatusTip("Redo last operation");
    m_redo_action->setEnabled(false);
    connect(m_redo_action, &QAction::triggered, this, &MainWindow::on_redo_clicked);
    edit_menu->addAction(m_redo_action);
    
    edit_menu->addSeparator();
    
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
    
    // Toolbar actions will be added when icons are available
}

void MainWindow::create_status_bar()
{
    // Status bar disabled - panels have their own status bars
    statusBar()->hide();
}

void MainWindow::create_panels()
{
    // Create Backup panel
    m_backup_panel = std::make_unique<BackupPanel>(this);
    m_tab_widget->addTab(m_backup_panel.get(), "Backup");
    
    // Create Organizer panel
    m_organizer_panel = std::make_unique<OrganizerPanel>(this);
    m_tab_widget->addTab(m_organizer_panel.get(), "Directory Organizer");
    
    // Create Duplicate Finder panel
    m_duplicate_finder_panel = std::make_unique<DuplicateFinderPanel>(this);
    m_tab_widget->addTab(m_duplicate_finder_panel.get(), "Duplicate Finder");
    
    // Create License Scanner panel
    m_license_scanner_panel = std::make_unique<LicenseScannerPanel>(this);
    m_tab_widget->addTab(m_license_scanner_panel.get(), "License Scanner");
    
    // Create App Migration panel
    m_app_migration_panel = std::make_unique<AppMigrationPanel>(this);
    m_tab_widget->addTab(m_app_migration_panel.get(), "App Migration");
    
    // Connect panel signals to main window status bar
    connect(m_backup_panel.get(), &BackupPanel::status_message,
            this, [this](const QString& msg) { update_status(msg, 5000); });
    // Backup panel has its own progress bar, no progress_updated signal needed
    
    connect(m_app_migration_panel.get(), &AppMigrationPanel::status_message,
            this, [this](const QString& msg) { update_status(msg, 5000); });
    connect(m_app_migration_panel.get(), &AppMigrationPanel::progress_updated,
            this, &MainWindow::update_progress);
    
    // Logger will be wired in Phase 3
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
        "<li>User Profile Backup & Restore</li>"
        "<li>Application Migration</li>"
        "<li>Directory Organization</li>"
        "<li>Duplicate File Detection</li>"
        "<li>License Key Scanner</li>"
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

void MainWindow::on_undo_clicked()
{
    sak::UndoManager::instance().undo();
}

void MainWindow::on_redo_clicked()
{
    sak::UndoManager::instance().redo();
}

void MainWindow::on_undo_available(bool can_undo)
{
    if (m_undo_action) {
        m_undo_action->setEnabled(can_undo);
        if (can_undo) {
            QString text = sak::UndoManager::instance().undoText();
            m_undo_action->setText(QString("&Undo %1").arg(text));
            m_undo_action->setStatusTip(QString("Undo: %1").arg(text));
        } else {
            m_undo_action->setText("&Undo");
            m_undo_action->setStatusTip("Nothing to undo");
        }
    }
}

void MainWindow::on_redo_available(bool can_redo)
{
    if (m_redo_action) {
        m_redo_action->setEnabled(can_redo);
        if (can_redo) {
            QString text = sak::UndoManager::instance().redoText();
            m_redo_action->setText(QString("&Redo %1").arg(text));
            m_redo_action->setStatusTip(QString("Redo: %1").arg(text));
        } else {
            m_redo_action->setText("&Redo");
            m_redo_action->setStatusTip("Nothing to redo");
        }
    }
}

void MainWindow::setup_undo_redo()
{
    auto& undo_manager = sak::UndoManager::instance();
    
    // Connect undo manager signals to UI updates
    connect(&undo_manager, &sak::UndoManager::canUndoChanged,
            this, &MainWindow::on_undo_available);
    connect(&undo_manager, &sak::UndoManager::canRedoChanged,
            this, &MainWindow::on_redo_available);
    connect(&undo_manager, &sak::UndoManager::undoTextChanged,
            this, [this](const QString&) { 
                on_undo_available(sak::UndoManager::instance().canUndo()); 
            });
    connect(&undo_manager, &sak::UndoManager::redoTextChanged,
            this, [this](const QString&) { 
                on_redo_available(sak::UndoManager::instance().canRedo()); 
            });
    
    // Initialize action states
    on_undo_available(false);
    on_redo_available(false);
}

void MainWindow::load_window_state()
{
    // Check for portable mode (portable.ini in app directory)
    QString appDir = QCoreApplication::applicationDirPath();
    QString portableMarker = appDir + "/portable.ini";
    
    std::unique_ptr<QSettings> settings;
    if (QFile::exists(portableMarker)) {
        settings = std::make_unique<QSettings>(appDir + "/settings.ini", QSettings::IniFormat);
    } else {
        settings = std::make_unique<QSettings>("SAK", "Utility");
    }
    
    restoreGeometry(settings->value("window/geometry").toByteArray());
    restoreState(settings->value("window/state").toByteArray());
    
    // Restore last active tab
    int last_tab = settings->value("window/active_tab", 0).toInt();
    if (last_tab >= 0 && last_tab < m_tab_widget->count()) {
        m_tab_widget->setCurrentIndex(last_tab);
    }
    
    // Logger will be wired in Phase 3
}

void MainWindow::save_window_state()
{
    // Check for portable mode (portable.ini in app directory)
    QString appDir = QCoreApplication::applicationDirPath();
    QString portableMarker = appDir + "/portable.ini";
    
    std::unique_ptr<QSettings> settings;
    if (QFile::exists(portableMarker)) {
        settings = std::make_unique<QSettings>(appDir + "/settings.ini", QSettings::IniFormat);
    } else {
        settings = std::make_unique<QSettings>("SAK", "Utility");
    }
    
    settings->setValue("window/geometry", saveGeometry());
    settings->setValue("window/state", saveState());
    settings->setValue("window/active_tab", m_tab_widget->currentIndex());
    
    // Logger will be wired in Phase 3
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Check if any operations are running
    // In Phase 3, we'll add worker thread checks
    
    event->accept();
}
