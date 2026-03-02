// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main_window.cpp
/// @brief Implements the main application window with tabbed panel navigation

#include "sak/main_window.h"
#include "sak/version.h"
#include "sak/user_migration_panel.h"
#include "sak/organizer_panel.h"
#include "sak/duplicate_finder_panel.h"
#include "sak/app_installation_panel.h"
#include "sak/image_flasher_panel.h"
#include "sak/quick_actions_panel.h"
#include "sak/network_transfer_panel.h"
#include "sak/diagnostic_benchmark_panel.h"
#include "sak/wifi_manager_panel.h"
#include "sak/detachable_log_window.h"
#include "sak/config_manager.h"
#include "sak/about_dialog.h"
#include "sak/style_constants.h"
#include "sak/layout_constants.h"

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
#include <QShortcut>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QDateTime>

namespace sak {

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
    setMinimumSize(sak::kMainWindowMinW, sak::kMainWindowMinH);
    resize(sak::kMainWindowInitW, sak::kMainWindowInitH);


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
    m_logWindow = new DetachableLogWindow(tr("S.A.K. Log"), this);

    createPanels();
    createKeyboardShortcuts();

    updateStatus("Ready", 0);
}

void MainWindow::createMenuBar()
{
    // Menu bar removed  --  all items moved to panels/tabs
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
    m_progress_bar->setMaximumWidth(sak::kProgressBarMaxW);
    m_progress_bar->setMaximumHeight(sak::kProgressBarMaxH);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setVisible(false);
    statusBar()->addPermanentWidget(m_progress_bar);
}

void MainWindow::createPanels()
{
    createToolPanels();
    createAboutPanel();
    connectPanelSignals();
    connectPanelLogs();
}

void MainWindow::createToolPanels()
{
    // Create Quick Actions panel (first tab)
    m_quick_actions_panel = std::make_unique<QuickActionsPanel>(this);
    m_tab_widget->addTab(m_quick_actions_panel.get(), "Quick Actions");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "Common system utilities and Quick Actions (Ctrl+1)");

    // Create User Migration panel
    m_user_migration_panel = std::make_unique<UserMigrationPanel>(this);
    m_tab_widget->addTab(m_user_migration_panel.get(), "Backup & Restore");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "Backup and restore user profiles (Ctrl+2)");

    // Create Organizer panel
    m_organizer_panel = std::make_unique<OrganizerPanel>(this);
    m_tab_widget->addTab(m_organizer_panel.get(), "Directory Organizer");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "Organize files in a directory by type (Ctrl+3)");

    // Create Duplicate Finder panel
    m_duplicate_finder_panel = std::make_unique<DuplicateFinderPanel>(this);
    m_tab_widget->addTab(m_duplicate_finder_panel.get(), "Duplicate Finder");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "Find and manage duplicate files (Ctrl+4)");

    // Create App Installation panel
    m_app_installation_panel = std::make_unique<AppInstallationPanel>(this);
    m_tab_widget->addTab(m_app_installation_panel.get(), "App Installation");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "Install applications via Chocolatey (Ctrl+5)");

    // Create Network Transfer panel
    if (ConfigManager::instance().getNetworkTransferEnabled()) {
        m_network_transfer_panel = std::make_unique<NetworkTransferPanel>(this);
        m_tab_widget->addTab(m_network_transfer_panel.get(), "Network Transfer");
        m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
            "Transfer user profiles across the network (Ctrl+6)");
    }

    // Create Image Flasher panel
    m_image_flasher_panel = std::make_unique<ImageFlasherPanel>(this);
    m_tab_widget->addTab(m_image_flasher_panel.get(), "Image Flasher");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "Flash ISO images to USB drives (Ctrl+7)");

    // Create Diagnostic & Benchmarking panel
    m_diagnostic_benchmark_panel = std::make_unique<DiagnosticBenchmarkPanel>(this);
    m_tab_widget->addTab(m_diagnostic_benchmark_panel.get(), "Diagnostics");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "System diagnostics, benchmarks, and stress tests (Ctrl+8)");

    // Create WiFi Manager panel
    m_wifi_manager_panel = std::make_unique<WifiManagerPanel>(this);
    m_tab_widget->addTab(m_wifi_manager_panel.get(), "WiFi Manager");
    m_tab_widget->setTabToolTip(m_tab_widget->count() - 1,
        "Manage WiFi networks, QR codes, and profiles (Ctrl+9)");
    connect(m_wifi_manager_panel.get(), &WifiManagerPanel::statusMessage,
            this, &MainWindow::updateStatus);
}

void MainWindow::loadAboutPanelIcon(QLabel* iconLabel)
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

void MainWindow::createAboutPanel()
{
    auto* aboutPanel = new QWidget(this);
    auto* aboutLayout = new QVBoxLayout(aboutPanel);
    aboutLayout->setSpacing(12);
    aboutLayout->setContentsMargins(16, 16, 16, 16);

        // Header  --  use splash screen image as icon
        auto* headerLayout = new QHBoxLayout();
        auto* iconLabel = new QLabel(aboutPanel);
        iconLabel->setFixedSize(sak::kIconSize, sak::kIconSize);
        iconLabel->setAccessibleName(QStringLiteral("S.A.K. Utility application icon"));
        loadAboutPanelIcon(iconLabel);
        headerLayout->addWidget(iconLabel);

        auto* titleLayout = new QVBoxLayout();
        auto* title = new QLabel("<b>S.A.K. Utility</b>", aboutPanel);
        title->setStyleSheet(QString("font-size: %1pt; font-weight: 700;").arg(ui::kFontSizeTitle));
        titleLayout->addWidget(title);
        auto* ver = new QLabel(
            QString("Version %1 \u2014 %2").arg(get_version(), get_build_date()), aboutPanel);
        ver->setStyleSheet(QString("font-size: %1pt; color: %2;").arg(ui::kFontSizeBody)
            .arg(ui::kColorTextMuted));
        titleLayout->addWidget(ver);
        headerLayout->addLayout(titleLayout);
        headerLayout->addStretch();
        aboutLayout->addLayout(headerLayout);

        // Tabs inside about panel  --  all use QTextBrowser for uniform look
        auto* aboutTabs = new QTabWidget(aboutPanel);

        // About tab (QTextBrowser with HTML  --  matches License tab style)
        auto* descBrowser = new QTextBrowser(aboutPanel);
        descBrowser->setOpenExternalLinks(true);
        descBrowser->setHtml(
            "<h3>Swiss Army Knife (S.A.K.) Utility</h3>"
            "<p><b>PC Technician's Toolkit for Windows Migration and Maintenance</b></p>"
            "<p>Designed for PC technicians who need to migrate systems, backup user profiles, "
            "and manage files efficiently. Built with modern C++23 and Qt6 for Windows 10/11 "
            "x64.</p>");
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

void MainWindow::connectPanelSignals()
{
    // Connect panel signals to main window status bar
    connect(m_quick_actions_panel.get(), &QuickActionsPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg, timeout_ms); });
    connect(m_quick_actions_panel.get(), &QuickActionsPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    connect(m_user_migration_panel.get(), &UserMigrationPanel::statusMessage,
            this, [this](const QString& msg) { updateStatus(msg, 5000); });

    connect(m_organizer_panel.get(), &OrganizerPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg,
                timeout_ms > 0 ? timeout_ms : 5000); });
    connect(m_organizer_panel.get(), &OrganizerPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    connect(m_duplicate_finder_panel.get(), &DuplicateFinderPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg,
                timeout_ms > 0 ? timeout_ms : 5000); });
    connect(m_duplicate_finder_panel.get(), &DuplicateFinderPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    connect(m_app_installation_panel.get(), &AppInstallationPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg,
                timeout_ms > 0 ? timeout_ms : 5000); });
    connect(m_app_installation_panel.get(), &AppInstallationPanel::progressUpdated,
            this, &MainWindow::updateProgress);

    connect(m_image_flasher_panel.get(), &ImageFlasherPanel::statusMessage,
            this, [this](const QString& msg) { updateStatus(msg, 5000); });
    connect(m_image_flasher_panel.get(), &ImageFlasherPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    if (m_network_transfer_panel) {
        connect(m_network_transfer_panel.get(), &NetworkTransferPanel::statusMessage,
            this, [this](const QString& msg) { updateStatus(msg, 5000); });
        connect(m_network_transfer_panel.get(), &NetworkTransferPanel::progressUpdate,
            this, &MainWindow::updateProgress);
    }

    connect(m_diagnostic_benchmark_panel.get(), &DiagnosticBenchmarkPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg,
                timeout_ms > 0 ? timeout_ms : 5000); });
    connect(m_diagnostic_benchmark_panel.get(), &DiagnosticBenchmarkPanel::progressUpdate,
            this, &MainWindow::updateProgress);
}

void MainWindow::connectPanelLogs()
{
    // Connect all panel log signals to the shared log window (panel-aware)
    auto connectLog = [this](auto* panel) {
        int tabIdx = m_tab_widget->indexOf(panel);
        connect(panel, &std::remove_pointer_t<decltype(panel)>::logOutput,
                this, [this, tabIdx](const QString& msg) {
                    QString formatted = QDateTime::currentDateTime().toString("[HH:mm:ss] ") + msg;
                    appendLogIfActive(tabIdx, formatted);
                });

        auto* toggle = panel->logToggle();
        if (toggle) {
            connect(toggle, &LogToggleSwitch::toggled,
                    m_logWindow, &DetachableLogWindow::setLogVisible);
            connect(m_logWindow, &DetachableLogWindow::visibilityChanged,
                    toggle, &LogToggleSwitch::setChecked);
        }
    };

    connectLog(m_quick_actions_panel.get());
    connectLog(m_user_migration_panel.get());
    connectLog(m_organizer_panel.get());
    connectLog(m_duplicate_finder_panel.get());
    connectLog(m_app_installation_panel.get());
    connectLog(m_image_flasher_panel.get());
    connectLog(m_diagnostic_benchmark_panel.get());

    if (m_network_transfer_panel) {
        connectLog(m_network_transfer_panel.get());
    }

    // Switch log content when tabs change
    connect(m_tab_widget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Re-populate log when the log window becomes visible
    connect(m_logWindow, &DetachableLogWindow::visibilityChanged,
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
    if (!m_progress_bar) return;

    m_progress_bar->setMaximum(maximum);
    m_progress_bar->setValue(current);

    // Auto-show when work starts, auto-hide when complete
    if (current >= maximum && maximum > 0) {
        // Hide after a brief delay so the user sees 100%
        QTimer::singleShot(sak::kTimerSplashMs, this, &MainWindow::hideProgressBarIfComplete);
    } else if (maximum > 0) {
        m_progress_bar->setVisible(true);
    }
}

void MainWindow::hideProgressBarIfComplete()
{
    if (m_progress_bar && m_progress_bar->value() >= m_progress_bar->maximum()) {
        m_progress_bar->setVisible(false);
    }
}

void MainWindow::appendLogIfActive(int tabIdx, const QString& formatted)
{
    m_panelLogs[tabIdx].append(formatted);
    if (m_tab_widget->currentIndex() == tabIdx && m_logWindow->isLogVisible()) {
        m_logWindow->logTextEdit()->append(formatted);
    }
}

void MainWindow::setProgressVisible(bool visible)
{
    if (m_progress_bar) {
        m_progress_bar->setVisible(visible);
    }
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

void MainWindow::createKeyboardShortcuts()
{
    // Tab navigation: Ctrl+1..9 switches to tab index 0..8
    for (int i = 0; i < qMin(m_tab_widget->count(), 9); ++i) {
        auto* shortcut = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 +
            i)), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, i]() {
            m_tab_widget->setCurrentIndex(i);
        });
    }

    // Ctrl+Tab / Ctrl+Shift+Tab: cycle through tabs
    auto* nextTab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), this);
    nextTab->setContext(Qt::ApplicationShortcut);
    connect(nextTab, &QShortcut::activated, this, [this]() {
        int next = (m_tab_widget->currentIndex() + 1) % m_tab_widget->count();
        m_tab_widget->setCurrentIndex(next);
    });

    auto* prevTab = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this);
    prevTab->setContext(Qt::ApplicationShortcut);
    connect(prevTab, &QShortcut::activated, this, [this]() {
        int prev = (m_tab_widget->currentIndex() - 1 +
            m_tab_widget->count()) % m_tab_widget->count();
        m_tab_widget->setCurrentIndex(prev);
    });

    // Ctrl+L: Toggle log panel visibility
    auto* toggleLog = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    toggleLog->setContext(Qt::ApplicationShortcut);
    connect(toggleLog, &QShortcut::activated, this, [this]() {
        if (m_logWindow) {
            m_logWindow->setLogVisible(!m_logWindow->isLogVisible());
        }
    });
}

void MainWindow::loadWindowState()
{
    auto& config = ConfigManager::instance();

    if (config.getRestoreWindowGeometry()) {
        restoreGeometry(config.getWindowGeometry());
        restoreState(config.getWindowState());
    }

    // Always start on Quick Actions tab (index 0)
    m_tab_widget->setCurrentIndex(0);
}

void MainWindow::saveWindowState()
{
    auto& config = ConfigManager::instance();
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

} // namespace sak
