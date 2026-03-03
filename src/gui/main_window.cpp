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
#include "sak/advanced_search_panel.h"
#include "sak/advanced_uninstall_panel.h"
#include "sak/network_diagnostic_panel.h"
#include "sak/detachable_log_window.h"
#include "sak/config_manager.h"
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
#include <QSysInfo>

namespace sak {

namespace {

constexpr char kAboutTabHtml[] = R"SAKABOUT(
<style>
    body { font-family: 'Segoe UI', sans-serif; margin: 8px; }
    h2 { color: #1e293b; margin-bottom: 4px; }
    .subtitle { color: #64748b; font-size: 10pt; margin-bottom: 12px; }
    .section { margin-bottom: 14px; }
    .section-title {
        font-weight: 700; font-size: 10pt; color: #3b82f6;
        border-bottom: 1px solid #e2e8f0; padding-bottom: 3px; margin-bottom: 6px;
    }
    ul { margin: 2px 0 0 16px; padding: 0; }
    li { margin-bottom: 3px; color: #334155; }
    a { color: #3b82f6; text-decoration: none; }
    a:hover { text-decoration: underline; }
    .footer { color: #94a3b8; font-size: 9pt; margin-top: 12px; border-top: 1px solid #e2e8f0; padding-top: 6px; }
</style>

<h2>Swiss Army Knife (S.A.K.) Utility</h2>
<div class="subtitle">A portable Windows toolkit for PC technicians, IT pros, and sysadmins.<br/>
Built with modern C++23 and Qt 6 for Windows 10/11 x64.</div>

<div class="section">
    <div class="section-title">Migration &amp; Backup</div>
    <ul>
        <li><b>User Profile Backup &amp; Restore</b> &mdash; Step-by-step wizards with smart filtering, per-user customization, AES-256 encryption, and NTFS permission handling</li>
        <li><b>Application Migration</b> &mdash; Scan installed apps, match to Chocolatey packages, export/import, and bulk-install on a new PC</li>
        <li><b>Network Transfer</b> &mdash; Peer-to-peer encrypted LAN migration with resume, multi-PC orchestrator mode, and AES-256-GCM per chunk</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Quick Actions</div>
    <ul>
        <li><b>System Optimization</b> &mdash; Disk cleanup, browser cache, defragment, startup programs, power settings, visual effects</li>
        <li><b>Quick Backups</b> &mdash; QuickBooks, browser profiles, Outlook, Sticky Notes, saved games, tax software, photo tools, dev configs, WiFi networks, wallpaper, printers</li>
        <li><b>Maintenance</b> &mdash; App updates, Windows Update, system file verification, disk checks, icon cache, network reset, print spooler</li>
        <li><b>Troubleshooting</b> &mdash; System reports, bloatware scan, network speed test, malware scan, Windows Store repair, audio fixes</li>
        <li><b>Emergency Recovery</b> &mdash; Restore points, registry export, settings screenshots, BitLocker key backup</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Diagnostics &amp; Benchmarking</div>
    <ul>
        <li><b>Hardware Inventory</b> &mdash; CPU, memory, storage, GPU, and OS details</li>
        <li><b>SMART Disk Health</b> &mdash; Drive health, temperature, power-on hours, and attribute monitoring via bundled smartmontools</li>
        <li><b>Benchmarks</b> &mdash; CPU (single/multi-thread), disk (sequential &amp; random I/O), memory (bandwidth &amp; latency)</li>
        <li><b>Stress Testing</b> &mdash; CPU, memory, and disk stress with real-time thermal monitoring and configurable auto-abort</li>
        <li><b>Report Export</b> &mdash; HTML, JSON, and CSV reports</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Imaging &amp; File Tools</div>
    <ul>
        <li><b>Image Flasher</b> &mdash; Flash ISOs/IMGs to USB with streaming decompression and system-drive protection</li>
        <li><b>Windows ISO Download</b> &mdash; Download directly from Microsoft via UUP Dump API</li>
        <li><b>Linux ISO Download</b> &mdash; Built-in distro catalog (Ubuntu, Mint, Kali, SystemRescue, Clonezilla, and more)</li>
        <li><b>Directory Organizer</b> &mdash; Sort files by extension into categorized subdirectories with preview mode</li>
        <li><b>Duplicate File Finder</b> &mdash; MD5 hash-based duplicate detection with size filtering</li>
        <li><b>WiFi Manager</b> &mdash; QR code generation, network scanning, bulk export, and connection scripts</li>
    </ul>
</div>

<div class="footer">
    100% portable &mdash; no installer required. Drop on a USB drive and go.<br/>
    <a href="https://github.com/RandyNorthrup/S.A.K.-Utility">GitHub</a> &middot; AGPL-3.0 license
</div>
)SAKABOUT";

constexpr char kLicenseTabHtml[] = R"SAKLICENSE(
<h3>GNU Affero General Public License v3.0</h3>
<p>Copyright &copy; 2025-2026 Randy Northrup</p>
<p>This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.</p>
<p>This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.</p>
<p>You should have received a copy of the GNU Affero General Public License
along with this program. If not, see
<a href="https://www.gnu.org/licenses/">https://www.gnu.org/licenses/</a>.</p>
<p><b>Note:</b> This application uses Qt Framework (LGPL v3), Chocolatey (Apache 2.0),
smartmontools (GPLv2), aria2 (GPLv2), wimlib (LGPL v3), 7-Zip (LGPL v2.1),
qrcodegen (MIT), and additional open-source libraries. See the Credits tab
for the full list.</p>
)SAKLICENSE";

constexpr char kCreditsTabHtml[] = R"SAKCREDITS(
<style>
    body { font-family: 'Segoe UI', sans-serif; margin: 8px; }
    h3 { color: #1e293b; margin-bottom: 6px; }
    .dep { margin-bottom: 10px; }
    .dep b { color: #334155; }
    .dep .desc { color: #475569; font-size: 9pt; }
    a { color: #3b82f6; text-decoration: none; }
    a:hover { text-decoration: underline; }
</style>

<h3>Development</h3>
<p><b>Lead Developer:</b> Randy Northrup</p>

<h3>Third-Party Components</h3>

<div class="dep">
    <b><a href="https://www.qt.io/">Qt Framework 6.5+</a></b> &mdash; LGPL v3<br/>
    <span class="desc">GUI framework, threading, networking, cryptography</span>
</div>
<div class="dep">
    <b><a href="https://www.nayuki.io/page/qr-code-generator-library">qrcodegen</a></b> &mdash; MIT (Project Nayuki)<br/>
    <span class="desc">QR code generation (bundled source)</span>
</div>
<div class="dep">
    <b><a href="https://www.smartmontools.org/">smartmontools</a></b> &mdash; GPLv2<br/>
    <span class="desc">SMART disk health analysis (bundled smartctl)</span>
</div>
<div class="dep">
    <b><a href="https://chocolatey.org/">Chocolatey</a></b> &mdash; Apache 2.0<br/>
    <span class="desc">Windows package manager for application migration</span>
</div>
<div class="dep">
    <b><a href="https://aria2.github.io/">aria2</a></b> &mdash; GPLv2<br/>
    <span class="desc">Multi-connection download manager for ISO downloads</span>
</div>
<div class="dep">
    <b><a href="https://wimlib.net/">wimlib</a></b> &mdash; LGPL v3 (Eric Biggers)<br/>
    <span class="desc">WIM image library for UUP-to-ISO conversion</span>
</div>
<div class="dep">
    <b><a href="https://www.7-zip.org/">7-Zip</a></b> &mdash; LGPL v2.1 (Igor Pavlov)<br/>
    <span class="desc">Archive tool for ISO extraction and compression</span>
</div>
<div class="dep">
    <b><a href="https://github.com/abbodi1406">uup-converter-wimlib</a></b> &mdash; abbodi1406<br/>
    <span class="desc">UUP-to-ISO converter scripts</span>
</div>
<div class="dep">
    <b><a href="https://www.zlib.net/">zlib</a></b> &mdash; zlib License<br/>
    <span class="desc">gzip compression</span>
</div>
<div class="dep">
    <b><a href="https://sourceware.org/bzip2/">bzip2</a></b> &mdash; BSD-style<br/>
    <span class="desc">bzip2 compression</span>
</div>
<div class="dep">
    <b><a href="https://tukaani.org/xz/">XZ Utils / liblzma</a></b> &mdash; 0BSD / Public Domain<br/>
    <span class="desc">LZMA compression</span>
</div>
<div class="dep">
    <b>Windows BCrypt API</b> &mdash; OS component<br/>
    <span class="desc">AES-256 encryption, PBKDF2, SHA-256 (FIPS 140-2 validated)</span>
</div>

<h3>Special Thanks</h3>
<p>To the C++ and Qt communities for their excellent documentation and support.</p>
<p>To Microsoft for Windows API documentation, PowerShell, Windows SDK, and ADK tools.</p>
)SAKCREDITS";

constexpr char kTooltipQuickActions[] =
    "Common system utilities and Quick Actions (Ctrl+1)";
constexpr char kTooltipUserMigration[] =
    "Backup and restore user profiles (Ctrl+2)";
constexpr char kTooltipOrganizer[] =
    "Organize files in a directory by type (Ctrl+3)";
constexpr char kTooltipDuplicateFinder[] =
    "Find and manage duplicate files (Ctrl+4)";
constexpr char kTooltipAppInstallation[] =
    "Install applications via Chocolatey (Ctrl+5)";
constexpr char kTooltipNetworkTransfer[] =
    "Transfer user profiles across the network (Ctrl+6)";
constexpr char kTooltipImageFlasher[] =
    "Flash ISO images to USB drives (Ctrl+7)";
constexpr char kTooltipDiagnostics[] =
    "System diagnostics, benchmarks, and stress tests (Ctrl+8)";
constexpr char kTooltipWifiManager[] =
    "Manage WiFi networks, QR codes, and profiles (Ctrl+9)";
constexpr char kTooltipAdvancedSearch[] =
    "Search file contents, metadata, archives, and binary data";
constexpr char kTooltipAdvancedUninstall[] =
    "Deep application removal with leftover scanning and batch uninstall";
constexpr char kTooltipNetworkDiagnostics[] =
    "Network adapter inspection, connectivity testing, DNS diagnostics, "
    "port scanning, and more";

void AddTabWithTooltip(
        QTabWidget* tabWidget,
        QWidget* panel,
    const char* tabTitle,
    const char* tooltip)
{
        Q_ASSERT(tabWidget);
        Q_ASSERT(panel);
    Q_ASSERT(tabTitle);
    Q_ASSERT(tooltip);

    const int idx = tabWidget->addTab(panel, QString::fromUtf8(tabTitle));
    tabWidget->setTabToolTip(idx, QString::fromUtf8(tooltip));

    // Set accessible name on the panel widget so screen readers
    // identify each tab's content area
    const QString title = QString::fromUtf8(tabTitle);
    panel->setAccessibleName(title + QStringLiteral(" panel"));
    panel->setAccessibleDescription(QString::fromUtf8(tooltip));

    // Also set accessible text on the tab bar tab via tab widget
    tabWidget->setTabWhatsThis(idx, QString::fromUtf8(tooltip));
}

QTextBrowser* CreateHtmlBrowser(QWidget* parent, const char* html)
{
        Q_ASSERT(parent);
        Q_ASSERT(html);

        auto* browser = new QTextBrowser(parent);
        browser->setOpenExternalLinks(true);
        browser->setHtml(QString::fromUtf8(html));
        return browser;
}

} // namespace

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
    Q_ASSERT(!objectName().isEmpty() || true);  // widget valid
    setWindowTitle("S.A.K. Utility - Swiss Army Knife Utility");
    setMinimumSize(sak::kMainWindowMinW, sak::kMainWindowMinH);
    resize(sak::kMainWindowInitW, sak::kMainWindowInitH);


    // Create central tab widget
    m_tab_widget = new QTabWidget(this);
    m_tab_widget->setTabPosition(QTabWidget::North);
    m_tab_widget->setDocumentMode(true);
    m_tab_widget->setUsesScrollButtons(true);  // Scroll tabs when window is narrow
    m_tab_widget->setElideMode(Qt::ElideNone); // Don't truncate tab labels
    m_tab_widget->setAccessibleName(tr("Main panel tabs"));
    m_tab_widget->setAccessibleDescription(
        tr("Tab bar for switching between application panels. "
           "Use Ctrl+1..9 for tabs 1-9, Ctrl+0 for tab 10, "
           "Ctrl+Shift+1..4 for tabs 11-14, "
           "or Ctrl+Tab / Ctrl+Shift+Tab to cycle."));
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
    Q_ASSERT(m_tab_widget);
    // Create Quick Actions panel (first tab)
    m_quick_actions_panel = std::make_unique<QuickActionsPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_quick_actions_panel.get(),
        "Quick Actions", kTooltipQuickActions);

    // Create User Migration panel
    m_user_migration_panel = std::make_unique<UserMigrationPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_user_migration_panel.get(),
        "Backup & Restore", kTooltipUserMigration);

    // Create Organizer panel
    m_organizer_panel = std::make_unique<OrganizerPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_organizer_panel.get(),
        "Directory Organizer", kTooltipOrganizer);

    // Create Duplicate Finder panel
    m_duplicate_finder_panel = std::make_unique<DuplicateFinderPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_duplicate_finder_panel.get(),
        "Duplicate Finder", kTooltipDuplicateFinder);

    // Create App Installation panel
    m_app_installation_panel = std::make_unique<AppInstallationPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_app_installation_panel.get(),
        "App Installation", kTooltipAppInstallation);

    // Create Network Transfer panel
    if (ConfigManager::instance().getNetworkTransferEnabled()) {
        m_network_transfer_panel = std::make_unique<NetworkTransferPanel>(this);
        AddTabWithTooltip(m_tab_widget, m_network_transfer_panel.get(),
            "Network Transfer", kTooltipNetworkTransfer);
    }

    // Create Image Flasher panel
    m_image_flasher_panel = std::make_unique<ImageFlasherPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_image_flasher_panel.get(),
        "Image Flasher", kTooltipImageFlasher);

    // Create Diagnostic & Benchmarking panel
    m_diagnostic_benchmark_panel = std::make_unique<DiagnosticBenchmarkPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_diagnostic_benchmark_panel.get(),
        "Diagnostics", kTooltipDiagnostics);

    // Create WiFi Manager panel
    m_wifi_manager_panel = std::make_unique<WifiManagerPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_wifi_manager_panel.get(),
        "WiFi Manager", kTooltipWifiManager);
    connect(m_wifi_manager_panel.get(), &WifiManagerPanel::statusMessage,
            this, &MainWindow::updateStatus);

    // Create Advanced Search panel
    m_advanced_search_panel = std::make_unique<AdvancedSearchPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_advanced_search_panel.get(),
        "Advanced Search", kTooltipAdvancedSearch);

    // Create Advanced Uninstall panel
    m_advanced_uninstall_panel = std::make_unique<AdvancedUninstallPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_advanced_uninstall_panel.get(),
        "Advanced Uninstall", kTooltipAdvancedUninstall);

    // Create Network Diagnostics panel
    m_network_diagnostic_panel = std::make_unique<NetworkDiagnosticPanel>(this);
    AddTabWithTooltip(m_tab_widget, m_network_diagnostic_panel.get(),
        "Network Diagnostics", kTooltipNetworkDiagnostics);
}

void MainWindow::loadAboutPanelIcon(QLabel* iconLabel)
{
    Q_ASSERT(iconLabel);
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
            QString("QLabel { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
                    "stop:0 %1,stop:1 %2); border-radius: 12px; }")
            .arg(ui::kColorPrimary, ui::kColorPrimaryDark));
    }
}

void MainWindow::createAboutPanel()
{
    Q_ASSERT(m_tab_widget);
    auto* aboutPanel = new QWidget(this);
    auto* aboutLayout = new QVBoxLayout(aboutPanel);
    aboutLayout->setSpacing(12);
    aboutLayout->setContentsMargins(ui::kMarginLarge, ui::kMarginLarge,
        ui::kMarginLarge, ui::kMarginLarge);

        // Header  --  use splash screen image as icon
        auto* headerLayout = new QHBoxLayout();
        auto* iconLabel = new QLabel(aboutPanel);
        iconLabel->setFixedSize(sak::kIconSize, sak::kIconSize);
        iconLabel->setAccessibleName(QStringLiteral("S.A.K. Utility application icon"));
        loadAboutPanelIcon(iconLabel);
        headerLayout->addWidget(iconLabel);

        auto* titleLayout = new QVBoxLayout();
        auto* title = new QLabel(QStringLiteral("<b>S.A.K. Utility</b>"), aboutPanel);
        title->setStyleSheet(
                QString("font-size: %1pt; font-weight: 700;").arg(ui::kFontSizeTitle));
        titleLayout->addWidget(title);

        auto* ver = new QLabel(
                QString("Version %1 \u2014 %2").arg(get_version(), get_build_date()), aboutPanel);
        ver->setStyleSheet(QString("font-size: %1pt; color: %2;")
                .arg(ui::kFontSizeBody)
                .arg(ui::kColorTextMuted));
        titleLayout->addWidget(ver);

        headerLayout->addLayout(titleLayout);
        headerLayout->addStretch();
        aboutLayout->addLayout(headerLayout);

        // Tabs inside about panel  --  all use QTextBrowser for uniform look
        auto* aboutTabs = new QTabWidget(aboutPanel);
        aboutTabs->addTab(CreateHtmlBrowser(aboutPanel, kAboutTabHtml), QStringLiteral("About"));
        aboutTabs->addTab(
            CreateHtmlBrowser(aboutPanel, kLicenseTabHtml),
            QStringLiteral("License"));
        aboutTabs->addTab(
            CreateHtmlBrowser(aboutPanel, kCreditsTabHtml),
            QStringLiteral("Credits"));

        aboutLayout->addWidget(aboutTabs);
        m_tab_widget->addTab(aboutPanel, QStringLiteral("About"));
}

void MainWindow::connectPanelSignals()
{
    Q_ASSERT(m_quick_actions_panel);
    Q_ASSERT(m_user_migration_panel);
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

    connect(m_advanced_search_panel.get(), &AdvancedSearchPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg,
                timeout_ms > 0 ? timeout_ms : 5000); });
    connect(m_advanced_search_panel.get(), &AdvancedSearchPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    connect(m_advanced_uninstall_panel.get(), &AdvancedUninstallPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg,
                timeout_ms > 0 ? timeout_ms : 5000); });
    connect(m_advanced_uninstall_panel.get(), &AdvancedUninstallPanel::progressUpdate,
            this, &MainWindow::updateProgress);

    connect(m_network_diagnostic_panel.get(), &NetworkDiagnosticPanel::statusMessage,
            this, [this](const QString& msg, int timeout_ms) { updateStatus(msg,
                timeout_ms > 0 ? timeout_ms : 5000); });
    connect(m_network_diagnostic_panel.get(), &NetworkDiagnosticPanel::progressUpdate,
            this, &MainWindow::updateProgress);
}

void MainWindow::connectPanelLogs()
{
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(m_logWindow);
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
    connectLog(m_advanced_search_panel.get());
    connectLog(m_advanced_uninstall_panel.get());
    connectLog(m_network_diagnostic_panel.get());

    if (m_wifi_manager_panel) {
        connectLog(m_wifi_manager_panel.get());
    }

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
    Q_ASSERT(m_status_label);
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
    Q_ASSERT(m_tab_widget);
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
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(index >= -1 && index < m_tab_widget->count());
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
    Q_ASSERT(m_tab_widget);
    const int tabCount = m_tab_widget->count();

    // Tab navigation: Ctrl+1..9 switches to tab index 0..8
    for (int i = 0; i < qMin(tabCount, 9); ++i) {
        auto* shortcut = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 +
            i)), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, i]() {
            m_tab_widget->setCurrentIndex(i);
        });
    }

    // Ctrl+0: switches to tab index 9 (10th tab) — browser convention
    if (tabCount > 9) {
        auto* tab10 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
        tab10->setContext(Qt::ApplicationShortcut);
        connect(tab10, &QShortcut::activated, this, [this]() {
            m_tab_widget->setCurrentIndex(9);
        });
    }

    // Ctrl+Shift+1..5: switches to tab index 10..14 (tabs beyond 10)
    for (int i = 0; i < qMin(tabCount - 10, 5); ++i) {
        const int tabIdx = 10 + i;
        if (tabIdx >= tabCount) break;
        auto* shortcut = new QShortcut(QKeySequence(
            Qt::CTRL | Qt::SHIFT | static_cast<Qt::Key>(Qt::Key_1 + i)), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, tabIdx]() {
            m_tab_widget->setCurrentIndex(tabIdx);
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
    Q_ASSERT(m_tab_widget);
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
    Q_ASSERT(event);
    event->accept();
}

void MainWindow::moveEvent(QMoveEvent* event)
{
    Q_ASSERT(event);
    QMainWindow::moveEvent(event);
    if (m_logWindow) {
        m_logWindow->repositionIfAnchored();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    Q_ASSERT(event);
    QMainWindow::resizeEvent(event);
    if (m_logWindow) {
        m_logWindow->repositionIfAnchored();
    }
}

} // namespace sak
