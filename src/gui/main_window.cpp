// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/main_window.h"
#include "sak/version.h"
#include "sak/user_migration_panel.h"
#include "sak/organizer_panel.h"
#include "sak/duplicate_finder_panel.h"
#include "sak/app_migration_panel.h"
#include "sak/image_flasher_panel.h"
#include "sak/quick_actions_panel.h"
#include "sak/network_transfer_panel.h"
#include "sak/diagnostic_benchmark_panel.h"
#include "sak/wifi_manager_panel.h"
#include "sak/detachable_log_window.h"
#include "sak/config_manager.h"
#include "sak/about_dialog.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QDateTime>

using sak::AppMigrationPanel;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setupUi();
    loadWindowState();
}

MainWindow::~MainWindow()
{
    saveWindowState();
}

void MainWindow::setupUi()
{
    setWindowTitle("S.A.K. Utility - Swiss Army Knife Utility");
    setMinimumSize(900, 600);
    resize(1200, 800);

    
    // Create central tab widget
    m_tab_widget = new QTabWidget(this);
    m_tab_widget->setTabPosition(QTabWidget::North);
    m_tab_widget->setDocumentMode(true);
    m_tab_widget->setUsesScrollButtons(true);  // Scroll tabs when window is narrow
    m_tab_widget->setElideMode(Qt::ElideNone); // Don't truncate tab labels
    setCentralWidget(m_tab_widget);
    
    // Create UI elements
    createMenuBar();
    createStatusBar();

    // Create shared log window BEFORE panels so connectLog() can reference it
    m_logWindow = new sak::DetachableLogWindow(tr("S.A.K. Log"), this);

    createPanels();
    
    updateStatus("Ready", 0);
}

void MainWindow::createMenuBar()
{
    // Menu bar removed â€” all items moved to panels/tabs
    menuBar()->hide();
}



void MainWindow::createStatusBar()
{
    // Persistent status label
    m_status_label = new QLabel("Ready", this);
    m_status_label->setContentsMargins(6, 0, 6, 0);
    statusBar()->addWidget(m_status_label, 1);
    
    // Progress bar (hidden by default)
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setMaximumWidth(250);
    m_progress_bar->setMaximumHeight(18);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setVisible(false);
    statusBar()->addPermanentWidget(m_progress_bar);
}

void MainWindow::createPanels()
{
    // Create Quick Actions panel (first tab)
    m_quick_actions_panel = std::make_unique<sak::QuickActionsPanel>(this);
    m_tab_widget->addTab(m_quick_actions_panel.get(), "Quick Actions");
    
    // Create User Migration panel
    m_user_migration_panel = std::make_unique<UserMigrationPanel>(this);
    m_tab_widget->addTab(m_user_migration_panel.get(), "User Migration");
    
    // Create Organizer panel
    m_organizer_panel = std::make_unique<OrganizerPanel>(this);
    m_tab_widget->addTab(m_organizer_panel.get(), "Directory Organizer");
    
    // Create Duplicate Finder panel
    m_duplicate_finder_panel = std::make_unique<DuplicateFinderPanel>(this);
    m_tab_widget->addTab(m_duplicate_finder_panel.get(), "Duplicate Finder");
    
    // Create App Installation panel
    m_app_migration_panel = std::make_unique<AppMigrationPanel>(this);
    m_tab_widget->addTab(m_app_migration_panel.get(), "App Installation");

    // Create Network Transfer panel
    if (sak::ConfigManager::instance().getNetworkTransferEnabled()) {
        m_network_transfer_panel = std::make_unique<sak::NetworkTransferPanel>(this);
        m_tab_widget->addTab(m_network_transfer_panel.get(), "Network Transfer");
    }
    
    // Create Image Flasher panel
    m_image_flasher_panel = std::make_unique<ImageFlasherPanel>(this);
    m_tab_widget->addTab(m_image_flasher_panel.get(), "Image Flasher");
    
    // Create Diagnostic & Benchmarking panel
    m_diagnostic_panel = std::make_unique<sak::DiagnosticBenchmarkPanel>(this);
    m_tab_widget->addTab(m_diagnostic_panel.get(), "Diagnostics");

    // Create WiFi Manager panel
    m_wifi_manager_panel = std::make_unique<sak::WifiManagerPanel>(this);
    m_tab_widget->addTab(m_wifi_manager_panel.get(), "WiFi Manager");
    connect(m_wifi_manager_panel.get(), &sak::WifiManagerPanel::statusMessage,
            this, &MainWindow::updateStatus);



    // Create About panel (embedded version of AboutDialog content)
    {
        auto* aboutPanel = new QWidget(this);
        auto* aboutLayout = new QVBoxLayout(aboutPanel);
        aboutLayout->setSpacing(12);
        aboutLayout->setContentsMargins(16, 16, 16, 16);

        // Header â€” use splash screen image as icon
        auto* headerLayout = new QHBoxLayout();
        auto* iconLabel = new QLabel(aboutPanel);
        iconLabel->setFixedSize(64, 64);
        {
            const QString appDir = QCoreApplication::applicationDirPath();
            const QStringList splashCandidates = {
                appDir + "/sak_splash.png",
                appDir + "/resources/sak_splash.png",
                appDir + "/../resources/sak_splash.png",
                appDir + "/../sak_splash.png"
            };
            QPixmap splashPix;
            for (const auto& path : splashCandidates) {
                if (QFileInfo::exists(path)) {
                    splashPix.load(path);
                    break;
                }
            }
            if (!splashPix.isNull()) {
                iconLabel->setPixmap(
                    splashPix.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            } else {
                iconLabel->setStyleSheet(
                    "QLabel { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
                    "stop:0 #3b82f6,stop:1 #2563eb); border-radius: 12px; }");
            }
        }
        headerLayout->addWidget(iconLabel);

        auto* titleLayout = new QVBoxLayout();
        auto* title = new QLabel("<b>S.A.K. Utility</b>", aboutPanel);
        title->setStyleSheet("font-size: 18pt; font-weight: 700;");
        titleLayout->addWidget(title);
        auto* ver = new QLabel(
            QString("Version %1 \u2014 %2").arg(sak::get_version(), sak::get_build_date()), aboutPanel);
        ver->setStyleSheet("font-size: 10pt; color: #64748b;");
        titleLayout->addWidget(ver);
        headerLayout->addLayout(titleLayout);
        headerLayout->addStretch();
        aboutLayout->addLayout(headerLayout);

        // Tabs inside about panel â€” all use QTextBrowser for uniform look
        auto* aboutTabs = new QTabWidget(aboutPanel);

        // About tab (QTextBrowser with HTML â€” matches License tab style)
        auto* descBrowser = new QTextBrowser(aboutPanel);
        descBrowser->setOpenExternalLinks(true);
        descBrowser->setHtml(
            "<h3>Swiss Army Knife (S.A.K.) Utility</h3>"
            "<p><b>PC Technician's Toolkit for Windows Migration and Maintenance</b></p>"
            "<p>Designed for PC technicians who need to migrate systems, backup user profiles, "
            "and manage files efficiently. Built with modern C++23 and Qt6 for Windows 10/11 x64.</p>");
        aboutTabs->addTab(descBrowser, "About");

        // License tab (QTextBrowser with HTML)
        auto* licBrowser = new QTextBrowser(aboutPanel);
        licBrowser->setOpenExternalLinks(true);
        licBrowser->setHtml(
            "<h3>GNU Affero General Public License v3.0</h3>"
            "<p>Copyright &copy; 2025 Randy Northrup</p>"
            "<p>This program is free software: you can redistribute it and/or modify "
            "it under the terms of the GNU Affero General Public License as published by "
            "the Free Software Foundation, either version 3 of the License, or "
            "(at your option) any later version.</p>"
            "<p>This program is distributed in the hope that it will be useful, "
            "but WITHOUT ANY WARRANTY; without even the implied warranty of "
            "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the "
            "GNU Affero General Public License for more details.</p>"
            "<p>You should have received a copy of the GNU Affero General Public License "
            "along with this program. If not, see "
            "<a href='https://www.gnu.org/licenses/'>https://www.gnu.org/licenses/</a>.</p>");
        aboutTabs->addTab(licBrowser, "License");

        aboutLayout->addWidget(aboutTabs);
        m_tab_widget->addTab(aboutPanel, "About");
    }
    
    // Connect panel signals to main window status bar
    connect(m_quick_actions_panel.get(), &sak::QuickActionsPanel::statusMessage,
            this, [this](const QString& msg, int timeout) { updateStatus(msg, timeout); });
    connect(m_quick_actions_panel.get(), &sak::QuickActionsPanel::progressUpdate,
            this, &MainWindow::updateProgress);
    
    connect(m_user_migration_panel.get(), &UserMigrationPanel::statusMessage,
            this, [this](const QString& msg) { updateStatus(msg, 5000); });
    
    connect(m_organizer_panel.get(), &OrganizerPanel::statusMessage,
            this, [this](const QString& msg, int timeout) { updateStatus(msg, timeout > 0 ? timeout : 5000); });
    connect(m_organizer_panel.get(), &OrganizerPanel::progressUpdate,
            this, &MainWindow::updateProgress);
    
    connect(m_duplicate_finder_panel.get(), &DuplicateFinderPanel::statusMessage,
            this, [this](const QString& msg, int timeout) { updateStatus(msg, timeout > 0 ? timeout : 5000); });
    connect(m_duplicate_finder_panel.get(), &DuplicateFinderPanel::progressUpdate,
            this, &MainWindow::updateProgress);
    
    connect(m_app_migration_panel.get(), &AppMigrationPanel::statusMessage,
            this, [this](const QString& msg, int timeout) { updateStatus(msg, timeout > 0 ? timeout : 5000); });
    connect(m_app_migration_panel.get(), &AppMigrationPanel::progressUpdated,
            this, &MainWindow::updateProgress);

    connect(m_image_flasher_panel.get(), &ImageFlasherPanel::statusMessage,
            this, [this](const QString& msg) { updateStatus(msg, 5000); });
    connect(m_image_flasher_panel.get(), &ImageFlasherPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    if (m_network_transfer_panel) {
        connect(m_network_transfer_panel.get(), &sak::NetworkTransferPanel::statusMessage,
            this, [this](const QString& msg) { updateStatus(msg, 5000); });
        connect(m_network_transfer_panel.get(), &sak::NetworkTransferPanel::progressUpdate,
            this, &MainWindow::updateProgress);
    }

    connect(m_diagnostic_panel.get(), &sak::DiagnosticBenchmarkPanel::statusMessage,
            this, [this](const QString& msg, int timeout) { updateStatus(msg, timeout > 0 ? timeout : 5000); });
    connect(m_diagnostic_panel.get(), &sak::DiagnosticBenchmarkPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    // Connect all panel log signals to the shared log window (panel-aware)
    auto connectLog = [this](auto* panel) {
        int tabIdx = m_tab_widget->indexOf(panel);
        connect(panel, &std::remove_pointer_t<decltype(panel)>::logOutput,
                this, [this, tabIdx](const QString& msg) {
                    QString formatted = QDateTime::currentDateTime().toString("[HH:mm:ss] ") + msg;
                    m_panelLogs[tabIdx].append(formatted);
                    if (m_tab_widget->currentIndex() == tabIdx && m_logWindow->isLogVisible()) {
                        m_logWindow->logTextEdit()->append(formatted);
                    }
                });

        auto* toggle = panel->logToggle();
        if (toggle) {
            connect(toggle, &sak::LogToggleSwitch::toggled,
                    m_logWindow, &sak::DetachableLogWindow::setLogVisible);
            connect(m_logWindow, &sak::DetachableLogWindow::visibilityChanged,
                    toggle, &sak::LogToggleSwitch::setChecked);
        }
    };

    connectLog(m_quick_actions_panel.get());
    connectLog(m_user_migration_panel.get());
    connectLog(m_organizer_panel.get());
    connectLog(m_duplicate_finder_panel.get());
    connectLog(m_app_migration_panel.get());
    connectLog(m_image_flasher_panel.get());
    connectLog(m_diagnostic_panel.get());

    if (m_network_transfer_panel) {
        connectLog(m_network_transfer_panel.get());
    }

    // Switch log content when tabs change
    connect(m_tab_widget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Re-populate log when the log window becomes visible
    connect(m_logWindow, &sak::DetachableLogWindow::visibilityChanged,
            this, [this](bool visible) {
                if (visible) {
                    onTabChanged(m_tab_widget->currentIndex());
                }
            });
    
}

void MainWindow::updateStatus(const QString& message, int timeout_ms)
{
    if (m_status_label) {
        if (timeout_ms > 0) {
            statusBar()->showMessage(message, timeout_ms);
        } else {
            m_status_label->setText(message);
        }
    }
}

void MainWindow::updateProgress(int current, int maximum)
{
    if (m_progress_bar) {
        m_progress_bar->setMaximum(maximum);
        m_progress_bar->setValue(current);

        // Auto-show when work starts, auto-hide when complete
        if (current >= maximum && maximum > 0) {
            // Hide after a brief delay so the user sees 100%
            QTimer::singleShot(1500, this, [this]() {
                if (m_progress_bar &&
                    m_progress_bar->value() >= m_progress_bar->maximum()) {
                    m_progress_bar->setVisible(false);
                }
            });
        } else if (maximum > 0) {
            m_progress_bar->setVisible(true);
        }
    }
}

void MainWindow::setProgressVisible(bool visible)
{
    if (m_progress_bar) {
        m_progress_bar->setVisible(visible);
    }
}

void MainWindow::onAboutClicked()
{
    // About is now a panel â€” no-op
}

void MainWindow::onTabChanged(int index)
{
    if (!m_logWindow) return;

    // Replace log content with the active panel's accumulated log
    m_logWindow->logTextEdit()->clear();
    if (m_panelLogs.contains(index)) {
        for (const auto& line : m_panelLogs[index]) {
            m_logWindow->logTextEdit()->append(line);
        }
    }
}

void MainWindow::onExitClicked()
{
    close();
}

void MainWindow::loadWindowState()
{
    auto& config = sak::ConfigManager::instance();
    
    if (config.getRestoreWindowGeometry()) {
        restoreGeometry(config.getWindowGeometry());
        restoreState(config.getWindowState());
    }
    
    // Always start on Quick Actions tab (index 0)
    m_tab_widget->setCurrentIndex(0);
}

void MainWindow::saveWindowState()
{
    auto& config = sak::ConfigManager::instance();
    config.setWindowGeometry(saveGeometry());
    config.setWindowState(saveState());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    event->accept();
}

void MainWindow::moveEvent(QMoveEvent* event)
{
    QMainWindow::moveEvent(event);
    if (m_logWindow) {
        m_logWindow->repositionIfAnchored();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (m_logWindow) {
        m_logWindow->repositionIfAnchored();
    }
}
