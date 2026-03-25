// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main_window.cpp
/// @brief Implements the main application window with tabbed panel navigation

#include "sak/main_window.h"

#include "sak/advanced_search_panel.h"
#include "sak/advanced_uninstall_panel.h"
#include "sak/app_installation_panel.h"
#include "sak/config_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/diagnostic_benchmark_panel.h"
#include "sak/email_inspector_panel.h"
#include "sak/image_flasher_panel.h"
#include "sak/layout_constants.h"
#include "sak/network_diagnostic_panel.h"
#include "sak/network_transfer_panel.h"
#include "sak/organizer_panel.h"
#include "sak/quick_actions_panel.h"
#include "sak/style_constants.h"
#include "sak/user_migration_panel.h"
#include "sak/version.h"
#include "sak/widget_helpers.h"
#include "sak/wifi_manager_panel.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QMoveEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QShortcut>
#include <QSysInfo>
#include <QTabBar>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

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
        <li><b>Network Transfer</b> &mdash; Peer-to-peer encrypted LAN migration with resume, multi-PC orchestrator mode, and AES-256-GCM per chunk</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Quick Actions</div>
    <ul>
        <li><b>System Optimization</b> &mdash; Disk cleanup, browser cache, defragment, startup programs, power settings, visual effects</li>
        <li><b>Quick Backups</b> &mdash; QuickBooks, browser profiles, Outlook, Sticky Notes, saved games, tax software, photo tools, dev configs, WiFi networks</li>
        <li><b>Maintenance</b> &mdash; App updates, Windows Update, system file verification, disk checks, icon cache, network reset, print spooler</li>
        <li><b>Troubleshooting</b> &mdash; System reports, bloatware scan, network speed test, malware scan, Windows Store repair, audio fixes</li>
        <li><b>Emergency Recovery</b> &mdash; Restore points, registry export, settings screenshots, wallpaper backup, BitLocker key backup, printer settings backup</li>
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
    <div class="section-title">File Management</div>
    <ul>
        <li><b>File Organizer</b> &mdash; Organize files by type with preview mode, customizable categories, and collision handling</li>
        <li><b>Duplicate Finder</b> &mdash; Content-based hash detection with parallel hashing and configurable minimum-size filtering</li>
        <li><b>Advanced Search</b> &mdash; Grep-style file content search with regex, binary/hex, image metadata, archive, and file metadata modes</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Application Management</div>
    <ul>
        <li><b>App Installation</b> &mdash; Scan installed apps, match to Chocolatey packages, export/import, and bulk-install on a new PC</li>
        <li><b>Advanced Uninstall</b> &mdash; Deep application removal with leftover scanning, registry snapshot diffs, recycle bin support, and locked-file reboot scheduling</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Imaging</div>
    <ul>
        <li><b>Image Flasher</b> &mdash; Flash ISOs/IMGs to USB with streaming decompression and system-drive protection</li>
        <li><b>Windows ISO Download</b> &mdash; Download directly from Microsoft via UUP Dump API</li>
        <li><b>Linux ISO Download</b> &mdash; Built-in distro catalog (Ubuntu, Mint, Kali, SystemRescue, Clonezilla, and more)</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Network Management</div>
    <ul>
        <li><b>Network Diagnostics</b> &mdash; Ping, traceroute, MTR, DNS lookup, port scan, bandwidth test, WiFi analyzer, active connections, firewall auditor, and network share browser</li>
        <li><b>Network Adapters</b> &mdash; Adapter inspector with ethernet configuration backup and restore across machines</li>
        <li><b>WiFi Manager</b> &mdash; QR code generation, network scanning, bulk export, and connection scripts</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Email &amp; Data Forensics</div>
    <ul>
        <li><b>PST/OST/MBOX Email Tool</b> &mdash; Offline forensic inspection of Outlook and Thunderbird email archives without client installation</li>
        <li><b>Email Search &amp; Export</b> &mdash; Full-text search across thousands of items with export to EML, ICS, VCF, and CSV formats</li>
        <li><b>Contacts Browser</b> &mdash; Searchable address book with sortable columns and export to VCF or CSV</li>
        <li><b>Calendar Viewer</b> &mdash; Month, week, and day views with event details and export to ICS or CSV</li>
        <li><b>Attachments Browser</b> &mdash; Scan all emails for attachments with type filtering, search, and batch extraction</li>
        <li><b>Email Profile Manager</b> &mdash; Backup and restore profiles for Outlook, Thunderbird, and Windows Mail</li>
        <li><b>Orphaned File Discovery</b> &mdash; Scan drives for orphaned PST, OST, and MBOX files not linked to active profiles</li>
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
smartmontools (GPLv2), aria2 (GPLv2), UUPMediaCreator (MIT), 7-Zip (LGPL v2.1),
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
    <b><a href="https://github.com/OSTooling/UUPMediaCreator">UUPMediaCreator</a></b> &mdash; MIT (OSTooling)<br/>
    <span class="desc">UUP-to-ISO converter (patched build, AppX provisioning skipped)</span>
</div>
<div class="dep">
    <b><a href="https://wimlib.net/">wimlib / libwim</a></b> &mdash; LGPL v3 (Eric Biggers)<br/>
    <span class="desc">WIM image library (bundled with UUPMediaConverter)</span>
</div>
<div class="dep">
    <b><a href="https://www.7-zip.org/">7-Zip</a></b> &mdash; LGPL v2.1 (Igor Pavlov)<br/>
    <span class="desc">Archive tool for ISO extraction and compression</span>
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

constexpr char kTooltipQuickActions[] = "Common system utilities and Quick Actions (Ctrl+1)";
constexpr char kTooltipUserMigration[] = "Backup and restore user profiles (Ctrl+2)";
constexpr char kTooltipOrganizer[] =
    "Organize files, find duplicates, and advanced search (Ctrl+3)";
constexpr char kTooltipAppManagement[] = "Install, uninstall, and manage applications (Ctrl+4)";
constexpr char kTooltipNetworkTransfer[] = "Transfer user profiles across the network (Ctrl+5)";
constexpr char kTooltipImageFlasher[] = "Flash ISO images to USB drives (Ctrl+6)";
constexpr char kTooltipDiagnostics[] = "System diagnostics, benchmarks, and stress tests (Ctrl+7)";
constexpr char kTooltipNetworkManagement[] =
    "Network diagnostics, WiFi management, and connectivity tools (Ctrl+8)";
constexpr char kTooltipEmailTool[] =
    "Inspect PST, OST, and MBOX email files — search, export, and manage profiles (Ctrl+9)";

const QString kDiscordBtnStyle = QStringLiteral(
    "QPushButton {"
    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 rgba(108, 117, 252, 0.92),"
    "    stop:0.5 rgba(88, 101, 242, 0.90),"
    "    stop:1 rgba(71, 82, 196, 0.88));"
    "  color: white; font-weight: 600;"
    "  padding: 8px 14px; border-radius: 10px;"
    "  border: 1px solid rgba(71, 82, 196, 0.7);"
    "}"
    "QPushButton:hover {"
    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 rgba(128, 137, 253, 0.95),"
    "    stop:0.5 rgba(109, 120, 247, 0.93),"
    "    stop:1 rgba(88, 101, 242, 0.90));"
    "}"
    "QPushButton:pressed {"
    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 rgba(71, 82, 196, 0.95),"
    "    stop:0.5 rgba(57, 66, 164, 0.93),"
    "    stop:1 rgba(45, 52, 140, 0.92));"
    "}");

constexpr int kTabIconSize = 20;

void AddTabWithTooltip(QTabWidget* tabWidget,
                       QWidget* panel,
                       const char* tabTitle,
                       const char* tooltip,
                       const char* iconPath = nullptr) {
    Q_ASSERT(tabWidget);
    Q_ASSERT(panel);
    Q_ASSERT(tabTitle);
    Q_ASSERT(tooltip);

    const QIcon icon = iconPath ? QIcon(QString::fromUtf8(iconPath)) : QIcon();
    const int idx = icon.isNull() ? tabWidget->addTab(panel, QString::fromUtf8(tabTitle))
                                  : tabWidget->addTab(panel, icon, QString::fromUtf8(tabTitle));
    if (!icon.isNull()) {
        tabWidget->setIconSize(QSize(kTabIconSize, kTabIconSize));
    }
    tabWidget->setTabToolTip(idx, QString::fromUtf8(tooltip));

    // Set accessible name on the panel widget so screen readers
    // identify each tab's content area
    const QString title = QString::fromUtf8(tabTitle);
    panel->setAccessibleName(title + QStringLiteral(" panel"));
    panel->setAccessibleDescription(QString::fromUtf8(tooltip));

    // Also set accessible text on the tab bar tab via tab widget
    tabWidget->setTabWhatsThis(idx, QString::fromUtf8(tooltip));
}

QTextBrowser* CreateHtmlBrowser(QWidget* parent, const char* html) {
    Q_ASSERT(parent);
    Q_ASSERT(html);

    auto* browser = new QTextBrowser(parent);
    browser->setOpenExternalLinks(true);
    browser->setHtml(QString::fromUtf8(html));
    return browser;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setupUi();
    loadWindowState();
}

MainWindow::~MainWindow() {
    saveWindowState();
}

void MainWindow::setupUi() {
    setWindowTitle("S.A.K. Utility - Swiss Army Knife Utility");
    setMinimumSize(sak::kMainWindowMinW, sak::kMainWindowMinH);
    resize(sak::kMainWindowInitW, sak::kMainWindowInitH);


    // Create central tab widget
    m_tab_widget = new QTabWidget(this);
    m_tab_widget->setTabPosition(QTabWidget::North);
    m_tab_widget->setDocumentMode(true);
    m_tab_widget->setUsesScrollButtons(true);   // Scroll tabs when window is narrow
    m_tab_widget->setElideMode(Qt::ElideNone);  // Don't truncate tab labels

    m_tab_widget->setAccessibleName(tr("Main panel tabs"));
    m_tab_widget->setAccessibleDescription(
        tr("Tab bar for switching between application panels. "
           "Use Ctrl+1..9 for tabs 1-9, Ctrl+0 for tab 10, "
           "or Ctrl+Tab / Ctrl+Shift+Tab to cycle."));
    setCentralWidget(m_tab_widget);

    // Create UI elements
    createMenuBar();
    createStatusBar();

    // Create shared log window BEFORE panels so connectLog() can reference it
    m_logWindow = new DetachableLogWindow(tr("S.A.K. Log"), this);

    createPanels();
    createKeyboardShortcuts();

    // Apply chevron icons to the tab-bar scroll buttons.
    // Qt creates the scroll QToolButtons lazily when tabs overflow, so we
    // re-apply after layout and on tab changes to cover all cases.
    QTimer::singleShot(0, this, &MainWindow::applyTabBarChevrons);
    connect(m_tab_widget, &QTabWidget::currentChanged, this, [this](int) {
        QTimer::singleShot(0, this, &MainWindow::applyTabBarChevrons);
    });

    // Enable mouse-wheel tab switching on the tab bar
    m_tab_widget->tabBar()->installEventFilter(this);

    updateStatus("Ready", 0);
}

void MainWindow::createMenuBar() {
    // Menu bar removed  --  all items moved to panels/tabs
    menuBar()->hide();
}


void MainWindow::createStatusBar() {
    // Persistent status label
    m_status_label = new QLabel("Ready", this);
    m_status_label->setContentsMargins(6, 0, 6, 0);
    statusBar()->addWidget(m_status_label, 1);

    // Progress bar (hidden by default, fixed size to prevent resizing)
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setFixedWidth(sak::kProgressBarMaxW);
    m_progress_bar->setFixedHeight(sak::kProgressBarMaxH);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setVisible(false);
    statusBar()->addPermanentWidget(m_progress_bar);
}

void MainWindow::createPanels() {
    createToolPanels();
    createHelpPanel();
    createAboutPanel();
    connectPanelSignals();
    connectPanelLogs();
}

void MainWindow::createToolPanels() {
    Q_ASSERT(m_tab_widget);
    createSimplePanels();
    createAppManagementPanel();
    createNetworkManagementPanel();
}

void MainWindow::createSimplePanels() {
    Q_ASSERT(m_tab_widget);
    // -- 1. Quick Actions ------------------------------------------------
    m_quick_actions_panel = std::make_unique<QuickActionsPanel>(this);
    AddTabWithTooltip(m_tab_widget,
                      m_quick_actions_panel.get(),
                      "Quick Actions",
                      kTooltipQuickActions,
                      ":/icons/icons/panel_quick_actions.svg");

    // -- 2. Backup and Restore -------------------------------------------
    m_user_migration_panel = std::make_unique<UserMigrationPanel>(this);
    AddTabWithTooltip(m_tab_widget,
                      m_user_migration_panel.get(),
                      "Backup and Restore",
                      kTooltipUserMigration,
                      ":/icons/icons/panel_backup_restore.svg");

    // -- 3. File Management (Organizer + Duplicate Finder + Advanced Search)
    m_organizer_panel = std::make_unique<OrganizerPanel>(this);
    m_advanced_search_panel = std::make_unique<AdvancedSearchPanel>(this);
    m_organizer_panel->tabWidget()->addTab(m_advanced_search_panel.get(), tr("Advanced Search"));
    AddTabWithTooltip(m_tab_widget,
                      m_organizer_panel.get(),
                      "File Management",
                      kTooltipOrganizer,
                      ":/icons/icons/panel_organizer.svg");

    // -- 4. Network Transfer (conditional) -------------------------------
    if (ConfigManager::instance().getNetworkTransferEnabled()) {
        m_network_transfer_panel = std::make_unique<NetworkTransferPanel>(this);
        AddTabWithTooltip(m_tab_widget,
                          m_network_transfer_panel.get(),
                          "Network Transfer",
                          kTooltipNetworkTransfer,
                          ":/icons/icons/panel_network_transfer.svg");
    }

    // -- 5. Image Flasher ------------------------------------------------
    m_image_flasher_panel = std::make_unique<ImageFlasherPanel>(this);
    AddTabWithTooltip(m_tab_widget,
                      m_image_flasher_panel.get(),
                      "Image Flasher",
                      kTooltipImageFlasher,
                      ":/icons/icons/panel_image_flasher.svg");

    // -- 6. Benchmark and Diagnostics ------------------------------------
    m_diagnostic_benchmark_panel = std::make_unique<DiagnosticBenchmarkPanel>(this);
    AddTabWithTooltip(m_tab_widget,
                      m_diagnostic_benchmark_panel.get(),
                      "Benchmark and Diagnostics",
                      kTooltipDiagnostics,
                      ":/icons/icons/panel_diagnostic.svg");

    // -- 7. Email Tool ----------------------------------------------------
    m_email_inspector_panel = std::make_unique<EmailInspectorPanel>(this);
    AddTabWithTooltip(m_tab_widget,
                      m_email_inspector_panel.get(),
                      "Email Tool",
                      kTooltipEmailTool,
                      ":/icons/icons/panel_email.svg");
}

void MainWindow::createAppManagementPanel() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(!m_app_installation_panel);
    m_app_installation_panel = std::make_unique<AppInstallationPanel>(this);
    m_advanced_uninstall_panel = std::make_unique<AdvancedUninstallPanel>(this);

    auto* appMgmtWrapper = new QWidget(this);
    auto* appMgmtLayout = new QVBoxLayout(appMgmtWrapper);
    appMgmtLayout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    appMgmtLayout->setSpacing(ui::kSpacingDefault);

    auto appHdr = sak::createDynamicPanelHeader(
        appMgmtWrapper,
        QStringLiteral(":/icons/icons/panel_app_install.svg"),
        tr("App Installation"),
        tr("Search, queue, and batch-install applications via Chocolatey"),
        appMgmtLayout);

    auto* appTabs = new QTabWidget(appMgmtWrapper);
    appTabs->addTab(m_app_installation_panel.get(), tr("App Installation"));
    appTabs->addTab(m_advanced_uninstall_panel.get(), tr("Advanced Uninstall"));
    appMgmtLayout->addWidget(appTabs, 1);

    connect(appTabs, &QTabWidget::currentChanged, this, [appHdr](int index) {
        struct TabMeta {
            const char* icon;
            const char* title;
            const char* subtitle;
        };
        static constexpr TabMeta kTabs[] = {
            {":/icons/icons/panel_app_install.svg",
             "App Installation",
             "Search, queue, and batch-install applications via Chocolatey"},
            {":/icons/icons/panel_uninstall.svg",
             "Advanced Uninstall",
             "Deep application removal with registry cleanup, leftover scanning, "
             "and batch uninstall support"},
        };
        if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
            const auto& m = kTabs[index];
            sak::updatePanelHeader(appHdr,
                                   QString::fromUtf8(m.icon),
                                   QCoreApplication::translate("MainWindow", m.title),
                                   QCoreApplication::translate("MainWindow", m.subtitle));
        }
    });

    AddTabWithTooltip(m_tab_widget,
                      appMgmtWrapper,
                      "Application Management",
                      kTooltipAppManagement,
                      ":/icons/icons/panel_app_install.svg");
}

void MainWindow::createNetworkManagementPanel() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(!m_network_diagnostic_panel);
    m_network_diagnostic_panel = std::make_unique<NetworkDiagnosticPanel>(this);
    m_wifi_manager_panel = std::make_unique<WifiManagerPanel>(this);
    connect(m_wifi_manager_panel.get(),
            &WifiManagerPanel::statusMessage,
            this,
            &MainWindow::updateStatus);

    auto* netMgmtWrapper = new QWidget(this);
    auto* netMgmtLayout = new QVBoxLayout(netMgmtWrapper);
    netMgmtLayout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    netMgmtLayout->setSpacing(ui::kSpacingDefault);

    auto netHdr = sak::createDynamicPanelHeader(
        netMgmtWrapper,
        QStringLiteral(":/icons/icons/panel_network.svg"),
        tr("Network Diagnostics & Troubleshooting"),
        tr("Comprehensive network analysis -- connectivity testing, DNS diagnostics, "
           "port scanning, bandwidth, WiFi analysis, firewall auditing, and more"),
        netMgmtLayout);

    auto* netTabs = new QTabWidget(netMgmtWrapper);
    netTabs->addTab(m_network_diagnostic_panel.get(), tr("Network Diagnostics"));
    netTabs->addTab(m_network_diagnostic_panel->adapterWidget(), tr("Network Adapters"));
    netTabs->addTab(m_wifi_manager_panel.get(), tr("WiFi Manager"));
    netMgmtLayout->addWidget(netTabs, 1);

    // Connect adapter-tab log toggle to the shared log window
    auto* adapterToggle = m_network_diagnostic_panel->adapterLogToggle();
    if (adapterToggle && m_logWindow) {
        connect(adapterToggle,
                &LogToggleSwitch::toggled,
                m_logWindow,
                &DetachableLogWindow::setLogVisible);
        connect(m_logWindow,
                &DetachableLogWindow::visibilityChanged,
                adapterToggle,
                &LogToggleSwitch::setChecked);
    }

    connect(netTabs, &QTabWidget::currentChanged, this, [netHdr](int index) {
        struct TabMeta {
            const char* icon;
            const char* title;
            const char* subtitle;
        };
        static constexpr TabMeta kTabs[] = {
            {":/icons/icons/panel_network.svg",
             "Network Diagnostics & Troubleshooting",
             "Comprehensive network analysis "
             "\xe2\x80\x94 connectivity testing, DNS diagnostics, "
             "port scanning, bandwidth, WiFi analysis, "
             "firewall auditing, and more"},
            {":/icons/icons/icons8-network-card.svg",
             "Network Adapters",
             "View and manage network adapter configurations, "
             "backup and restore Ethernet settings"},
            {":/icons/icons/panel_wifi.svg",
             "WiFi Manager",
             "Manage, share, and deploy Wi-Fi network profiles"},
        };
        if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
            const auto& m = kTabs[index];
            sak::updatePanelHeader(netHdr,
                                   QString::fromUtf8(m.icon),
                                   QCoreApplication::translate("MainWindow", m.title),
                                   QCoreApplication::translate("MainWindow", m.subtitle));
        }
    });

    AddTabWithTooltip(m_tab_widget,
                      netMgmtWrapper,
                      "Network Management",
                      kTooltipNetworkManagement,
                      ":/icons/icons/panel_network.svg");
}

void MainWindow::loadAboutPanelIcon(QLabel* iconLabel) {
    Q_ASSERT(iconLabel);
    Q_ASSERT(!QCoreApplication::applicationDirPath().isEmpty());
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList splashCandidates = {appDir + "/sak_splash.png",
                                          appDir + "/resources/sak_splash.png",
                                          appDir + "/../resources/sak_splash.png",
                                          appDir + "/../sak_splash.png"};
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
        iconLabel->setStyleSheet(QString("QLabel { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
                                         "stop:0 %1,stop:1 %2); border-radius: 12px; }")
                                     .arg(ui::kColorPrimary, ui::kColorPrimaryDark));
    }
}

void MainWindow::createAboutPanel() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(!QCoreApplication::applicationVersion().isEmpty());
    auto* aboutPanel = new QWidget(this);
    auto* aboutLayout = new QVBoxLayout(aboutPanel);
    aboutLayout->setSpacing(12);
    aboutLayout->setContentsMargins(
        ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge);

    // Header  --  use splash screen image as icon
    auto* headerLayout = new QHBoxLayout();
    auto* iconLabel = new QLabel(aboutPanel);
    iconLabel->setFixedSize(sak::kIconSize, sak::kIconSize);
    iconLabel->setAccessibleName(QStringLiteral("S.A.K. Utility application icon"));
    loadAboutPanelIcon(iconLabel);
    headerLayout->addWidget(iconLabel);

    auto* titleLayout = new QVBoxLayout();
    auto* title = new QLabel(QStringLiteral("<b>S.A.K. Utility</b>"), aboutPanel);
    title->setStyleSheet(QString("font-size: %1pt; font-weight: 700;").arg(ui::kFontSizeTitle));
    titleLayout->addWidget(title);

    auto* ver = new QLabel(QString("Version %1 \u2014 %2").arg(get_version(), get_build_date()),
                           aboutPanel);
    ver->setStyleSheet(
        QString("font-size: %1pt; color: %2;").arg(ui::kFontSizeBody).arg(ui::kColorTextMuted));
    titleLayout->addWidget(ver);

    headerLayout->addLayout(titleLayout);
    headerLayout->addStretch();
    aboutLayout->addLayout(headerLayout);

    // Tabs inside about panel  --  all use QTextBrowser for uniform look
    auto* aboutTabs = new QTabWidget(aboutPanel);
    aboutTabs->addTab(CreateHtmlBrowser(aboutPanel, kAboutTabHtml), QStringLiteral("About"));
    aboutTabs->addTab(CreateHtmlBrowser(aboutPanel, kLicenseTabHtml), QStringLiteral("License"));
    aboutTabs->addTab(CreateHtmlBrowser(aboutPanel, kCreditsTabHtml), QStringLiteral("Credits"));

    aboutLayout->addWidget(aboutTabs);
    const int aboutIdx =
        m_tab_widget->addTab(aboutPanel,
                             QIcon(QStringLiteral(":/icons/icons/panel_about.svg")),
                             QStringLiteral("About"));
    m_tab_widget->setIconSize(QSize(kTabIconSize, kTabIconSize));
    Q_UNUSED(aboutIdx);
}

void MainWindow::createHelpPanel() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(!QCoreApplication::applicationName().isEmpty());

    auto* helpPanel = new QWidget(this);
    auto* helpLayout = new QVBoxLayout(helpPanel);
    helpLayout->setSpacing(ui::kSpacingLarge);
    helpLayout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);

    // Panel header -- consistent with other panels
    auto* headerWidget = new QWidget(helpPanel);
    auto* headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    sak::createPanelHeader(headerWidget,
                           QStringLiteral(":/icons/icons/panel_help.svg"),
                           tr("Help and Support"),
                           tr("Get help, report issues, or request features for S.A.K. Utility."),
                           headerLayout);
    helpLayout->addWidget(headerWidget);

    // -- Shared card styles ----------------------------------------------
    const QString cardStyle = QString(
                                  "QFrame {"
                                  "  background-color: %1;"
                                  "  border: 1px solid %2;"
                                  "  border-radius: 10px;"
                                  "  padding: %3px;"
                                  "}"
                                  "QFrame:hover {"
                                  "  border-color: %4;"
                                  "}")
                                  .arg(ui::kColorBgWhite)
                                  .arg(ui::kColorBorderDefault)
                                  .arg(ui::kMarginMedium)
                                  .arg(ui::kColorPrimary);

    const QString titleStyle = QString(
                                   "font-size: %1pt; font-weight: 700; color: %2;"
                                   " border: none; background: transparent;")
                                   .arg(ui::kFontSizeSection)
                                   .arg(ui::kColorTextHeading);

    const QString descStyle = QString(
                                  "font-size: %1pt; color: %2;"
                                  " border: none; background: transparent;")
                                  .arg(ui::kFontSizeBody)
                                  .arg(ui::kColorTextSecondary);

    const QString logoStyle = QStringLiteral("border: none; background: transparent;");

    helpLayout->addLayout(
        createHelpRow_requestsAndBugs(helpPanel, cardStyle, titleStyle, descStyle, logoStyle));

    helpLayout->addLayout(
        createHelpRow_wikiAndCommunity(helpPanel, cardStyle, titleStyle, descStyle, logoStyle));

    helpLayout->addStretch();

    const int helpIdx = m_tab_widget->addTab(helpPanel,
                                             QIcon(QStringLiteral(":/icons/icons/panel_help.svg")),
                                             QStringLiteral("Help and Support"));
    m_tab_widget->setIconSize(QSize(kTabIconSize, kTabIconSize));
    Q_UNUSED(helpIdx);
}

// ============================================================================
// Help Panel -- Card Row Builders
// ============================================================================

QHBoxLayout* MainWindow::createHelpRow_requestsAndBugs(QWidget* parent,
                                                       const QString& cardStyle,
                                                       const QString& titleStyle,
                                                       const QString& descStyle,
                                                       const QString& logoStyle) {
    constexpr int kLogoSize = 48;

    auto makeCard = [&](const QString& iconPath,
                        const QString& title,
                        const QString& description,
                        QPushButton* button) -> QFrame* {
        auto* card = new QFrame(parent);
        card->setStyleSheet(cardStyle);
        auto* layout = new QVBoxLayout(card);
        layout->setSpacing(ui::kSpacingMedium);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* logo = new QLabel(card);
        logo->setPixmap(QIcon(iconPath).pixmap(kLogoSize, kLogoSize));
        logo->setAlignment(Qt::AlignCenter);
        logo->setStyleSheet(logoStyle);
        layout->addWidget(logo);

        auto* titleLabel = new QLabel(title, card);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet(titleStyle);
        layout->addWidget(titleLabel);

        auto* descLabel = new QLabel(description, card);
        descLabel->setWordWrap(true);
        descLabel->setAlignment(Qt::AlignCenter);
        descLabel->setStyleSheet(descStyle);
        layout->addWidget(descLabel);

        layout->addStretch();
        button->setParent(card);
        layout->addWidget(button);
        return card;
    };

    auto* row = new QHBoxLayout();
    row->setSpacing(ui::kSpacingLarge);

    auto* featureBtn = new QPushButton(tr("Request a Feature"), parent);
    featureBtn->setMinimumHeight(sak::kButtonHeightTall);
    featureBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    featureBtn->setAccessibleName(QStringLiteral("Submit feature request on GitHub"));
    featureBtn->setToolTip(QStringLiteral("Opens a GitHub issue form to submit a feature request"));
    connect(featureBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/RandyNorthrup/S.A.K.-Utility/issues/new"
                                "?template=feature_request.yml&title=%5BFeature+Request%5D%3A+")));
    });

    row->addWidget(makeCard(QStringLiteral(":/icons/icons/features.svg"),
                            tr("Feature Requests"),
                            tr("Have an idea to improve S.A.K. Utility?"
                               " Submit a feature request on GitHub."),
                            featureBtn));

    auto* bugBtn = new QPushButton(tr("Report a Bug"), parent);
    bugBtn->setMinimumHeight(sak::kButtonHeightTall);
    bugBtn->setStyleSheet(ui::kDangerButtonStyle);
    bugBtn->setAccessibleName(QStringLiteral("Report a bug on GitHub"));
    bugBtn->setToolTip(QStringLiteral("Opens a GitHub issue form to report a bug"));
    connect(bugBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/RandyNorthrup/S.A.K.-Utility/issues/new"
                                "?template=bug_report.yml&title=%5BBug%5D%3A+")));
    });

    row->addWidget(makeCard(QStringLiteral(":/icons/icons/bugs.svg"),
                            tr("Report a Bug"),
                            tr("Found something broken? Let us know so we can fix it."),
                            bugBtn));

    return row;
}

QHBoxLayout* MainWindow::createHelpRow_wikiAndCommunity(QWidget* parent,
                                                        const QString& cardStyle,
                                                        const QString& titleStyle,
                                                        const QString& descStyle,
                                                        const QString& logoStyle) {
    Q_ASSERT(parent);
    Q_ASSERT(!cardStyle.isEmpty());
    constexpr int kLogoSize = 48;

    auto makeCard = [&](const QString& iconPath,
                        const QString& title,
                        const QString& description,
                        QPushButton* button) -> QFrame* {
        auto* card = new QFrame(parent);
        card->setStyleSheet(cardStyle);
        auto* layout = new QVBoxLayout(card);
        layout->setSpacing(ui::kSpacingMedium);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* logo = new QLabel(card);
        logo->setPixmap(QIcon(iconPath).pixmap(kLogoSize, kLogoSize));
        logo->setAlignment(Qt::AlignCenter);
        logo->setStyleSheet(logoStyle);
        layout->addWidget(logo);

        auto* titleLabel = new QLabel(title, card);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet(titleStyle);
        layout->addWidget(titleLabel);

        auto* descLabel = new QLabel(description, card);
        descLabel->setWordWrap(true);
        descLabel->setAlignment(Qt::AlignCenter);
        descLabel->setStyleSheet(descStyle);
        layout->addWidget(descLabel);

        layout->addStretch();
        button->setParent(card);
        layout->addWidget(button);
        return card;
    };

    auto* row = new QHBoxLayout();
    row->setSpacing(ui::kSpacingLarge);

    auto* wikiBtn = new QPushButton(tr("Open Help Wiki"), parent);
    wikiBtn->setMinimumHeight(sak::kButtonHeightTall);
    wikiBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    wikiBtn->setAccessibleName(QStringLiteral("Open help wiki on GitHub"));
    wikiBtn->setToolTip(QStringLiteral("Opens the S.A.K. Utility wiki on GitHub"));
    connect(wikiBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/RandyNorthrup/S.A.K.-Utility/wiki")));
    });

    row->addWidget(makeCard(QStringLiteral(":/icons/icons/help.svg"),
                            tr("Help & Documentation"),
                            tr("Browse the wiki for guides, FAQ, and troubleshooting."),
                            wikiBtn));

    row->addWidget(createCommunityCard(parent, cardStyle, titleStyle, descStyle, logoStyle));

    return row;
}

QFrame* MainWindow::createCommunityCard(QWidget* parent,
                                        const QString& cardStyle,
                                        const QString& titleStyle,
                                        const QString& descStyle,
                                        const QString& logoStyle) {
    Q_ASSERT(parent);
    Q_ASSERT(!cardStyle.isEmpty());
    constexpr int kLogoSize = 48;

    auto* communityCard = new QFrame(parent);
    communityCard->setStyleSheet(cardStyle);
    auto* communityLayout = new QVBoxLayout(communityCard);
    communityLayout->setSpacing(ui::kSpacingMedium);
    communityLayout->setContentsMargins(0, 0, 0, 0);

    auto* communityLogo = new QLabel(communityCard);
    communityLogo->setPixmap(
        QIcon(QStringLiteral(":/icons/icons/discord.svg")).pixmap(kLogoSize, kLogoSize));
    communityLogo->setAlignment(Qt::AlignCenter);
    communityLogo->setStyleSheet(logoStyle);
    communityLayout->addWidget(communityLogo);

    auto* communityTitle = new QLabel(tr("Community"), communityCard);
    communityTitle->setAlignment(Qt::AlignCenter);
    communityTitle->setStyleSheet(titleStyle);
    communityLayout->addWidget(communityTitle);

    auto* communityDesc = new QLabel(tr("Join the Discord server for general discussion,"
                                        " help, and announcements."),
                                     communityCard);
    communityDesc->setWordWrap(true);
    communityDesc->setAlignment(Qt::AlignCenter);
    communityDesc->setStyleSheet(descStyle);
    communityLayout->addWidget(communityDesc);

    communityLayout->addStretch();

    auto* discordBtn = new QPushButton(tr("Join Discord"), communityCard);
    discordBtn->setMinimumHeight(sak::kButtonHeightTall);
    discordBtn->setStyleSheet(kDiscordBtnStyle);
    discordBtn->setAccessibleName(QStringLiteral("Join S.A.K. Utility Discord server"));
    discordBtn->setToolTip(QStringLiteral("Opens the general discussion Discord channel"));
    communityLayout->addWidget(discordBtn);
    connect(discordBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://discord.gg/pMh2n9kSK3")));
    });

    return communityCard;
}

void MainWindow::connectPanelSignals() {
    Q_ASSERT(m_quick_actions_panel);
    Q_ASSERT(m_user_migration_panel);
    // Connect panel signals to main window status bar
    connect(m_quick_actions_panel.get(),
            &QuickActionsPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) { updateStatus(msg, timeout_ms); });
    connect(m_quick_actions_panel.get(),
            &QuickActionsPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);

    connect(m_user_migration_panel.get(),
            &UserMigrationPanel::statusMessage,
            this,
            [this](const QString& msg) { updateStatus(msg, 5000); });

    connect(m_organizer_panel.get(),
            &OrganizerPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : 5000);
            });
    connect(m_organizer_panel.get(),
            &OrganizerPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);

    connect(m_app_installation_panel.get(),
            &AppInstallationPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : 5000);
            });
    connect(m_app_installation_panel.get(),
            &AppInstallationPanel::progressUpdated,
            this,
            &MainWindow::updateProgress);

    connect(m_image_flasher_panel.get(),
            &ImageFlasherPanel::statusMessage,
            this,
            [this](const QString& msg) { updateStatus(msg, 5000); });
    connect(m_image_flasher_panel.get(),
            &ImageFlasherPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);

    connectRemainingPanelSignals();
}

void MainWindow::connectRemainingPanelSignals() {
    Q_ASSERT(m_network_diagnostic_panel);
    Q_ASSERT(m_diagnostic_benchmark_panel);

    if (m_network_transfer_panel) {
        connect(m_network_transfer_panel.get(),
                &NetworkTransferPanel::statusMessage,
                this,
                [this](const QString& msg) { updateStatus(msg, 5000); });
        connect(m_network_transfer_panel.get(),
                &NetworkTransferPanel::progressUpdate,
                this,
                &MainWindow::updateProgress);
    }

    connect(m_diagnostic_benchmark_panel.get(),
            &DiagnosticBenchmarkPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : 5000);
            });
    connect(m_diagnostic_benchmark_panel.get(),
            &DiagnosticBenchmarkPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);

    connect(m_advanced_search_panel.get(),
            &AdvancedSearchPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : 5000);
            });
    connect(m_advanced_search_panel.get(),
            &AdvancedSearchPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);

    connect(m_advanced_uninstall_panel.get(),
            &AdvancedUninstallPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : 5000);
            });
    connect(m_advanced_uninstall_panel.get(),
            &AdvancedUninstallPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);

    connect(m_network_diagnostic_panel.get(),
            &NetworkDiagnosticPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : 5000);
            });
    connect(m_network_diagnostic_panel.get(),
            &NetworkDiagnosticPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);

    connect(m_email_inspector_panel.get(),
            &EmailInspectorPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : 5000);
            });
    connect(m_email_inspector_panel.get(),
            &EmailInspectorPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
}

int MainWindow::findPanelTabIndex(QWidget* panel) const {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(panel);

    int idx = m_tab_widget->indexOf(panel);
    if (idx >= 0) {
        return idx;
    }
    // Walk up through parent widgets to find the wrapper
    // that is a direct child of the main tab widget.
    QWidget* widget = panel->parentWidget();
    while (widget && widget != m_tab_widget) {
        idx = m_tab_widget->indexOf(widget);
        if (idx >= 0) {
            return idx;
        }
        widget = widget->parentWidget();
    }
    return -1;
}

void MainWindow::connectPanelLogs() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(m_logWindow);

    // Connect all panel log signals to the shared log window (panel-aware)
    auto connectLog = [this](auto* panel) {
        int tabIdx = findPanelTabIndex(panel);
        connect(panel,
                &std::remove_pointer_t<decltype(panel)>::logOutput,
                this,
                [this, tabIdx](const QString& msg) {
                    QString formatted = QDateTime::currentDateTime().toString("[HH:mm:ss] ") + msg;
                    appendLogIfActive(tabIdx, formatted);
                });

        auto* toggle = panel->logToggle();
        if (toggle) {
            connect(toggle,
                    &LogToggleSwitch::toggled,
                    m_logWindow,
                    &DetachableLogWindow::setLogVisible);
            connect(m_logWindow,
                    &DetachableLogWindow::visibilityChanged,
                    toggle,
                    &LogToggleSwitch::setChecked);
        }
    };

    connectLog(m_quick_actions_panel.get());
    connectLog(m_user_migration_panel.get());
    connectLog(m_organizer_panel.get());
    connectLog(m_app_installation_panel.get());
    connectLog(m_image_flasher_panel.get());
    connectLog(m_diagnostic_benchmark_panel.get());
    connectLog(m_advanced_search_panel.get());
    connectLog(m_advanced_uninstall_panel.get());
    connectLog(m_network_diagnostic_panel.get());
    connectLog(m_email_inspector_panel.get());

    if (m_wifi_manager_panel) {
        connectLog(m_wifi_manager_panel.get());
    }

    if (m_network_transfer_panel) {
        connectLog(m_network_transfer_panel.get());
    }

    // Switch log content when tabs change
    connect(m_tab_widget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Re-populate log when the log window becomes visible
    connect(m_logWindow, &DetachableLogWindow::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            onTabChanged(m_tab_widget->currentIndex());
        }
    });
}

void MainWindow::updateStatus(const QString& message, int timeout_ms) {
    Q_ASSERT(m_status_label);
    if (m_status_label) {
        if (timeout_ms > 0) {
            statusBar()->showMessage(message, timeout_ms);
        } else {
            m_status_label->setText(message);
        }
    }
}

void MainWindow::updateProgress(int current, int maximum) {
    Q_ASSERT(maximum >= 0);
    Q_ASSERT(current >= 0);
    Q_ASSERT(m_progress_bar);
    if (!m_progress_bar) {
        return;
    }

    m_progress_bar->setMaximum(maximum);
    m_progress_bar->setValue(current);

    // Auto-show when work starts, auto-hide when complete
    if (current == 0 && maximum == 0) {
        // Indeterminate progress (busy indicator)
        m_progress_bar->setVisible(true);
    } else if (current >= maximum && maximum > 0) {
        // Hide after a brief delay so the user sees 100%
        QTimer::singleShot(sak::kTimerSplashMs, this, &MainWindow::hideProgressBarIfComplete);
    } else if (maximum > 0) {
        m_progress_bar->setVisible(true);
    }
}

void MainWindow::hideProgressBarIfComplete() {
    if (m_progress_bar && m_progress_bar->value() >= m_progress_bar->maximum()) {
        m_progress_bar->setVisible(false);
    }
}

void MainWindow::appendLogIfActive(int tabIdx, const QString& formatted) {
    Q_ASSERT(m_tab_widget);
    m_panelLogs[tabIdx].append(formatted);
    if (m_tab_widget->currentIndex() == tabIdx && m_logWindow->isLogVisible()) {
        m_logWindow->logTextEdit()->append(formatted);
    }
}

void MainWindow::setProgressVisible(bool visible) {
    if (m_progress_bar) {
        m_progress_bar->setVisible(visible);
    }
}


void MainWindow::onTabChanged(int index) {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(index >= -1 && index < m_tab_widget->count());
    if (!m_logWindow) {
        return;
    }

    // Replace log content with the active panel's accumulated log
    m_logWindow->logTextEdit()->clear();
    if (m_panelLogs.contains(index)) {
        for (const auto& line : m_panelLogs[index]) {
            m_logWindow->logTextEdit()->append(line);
        }
    }
}

void MainWindow::createKeyboardShortcuts() {
    Q_ASSERT(m_logWindow);
    Q_ASSERT(m_tab_widget);
    const int tabCount = m_tab_widget->count();

    // Tab navigation: Ctrl+1..9 switches to tab index 0..8
    for (int i = 0; i < qMin(tabCount, 9); ++i) {
        auto* shortcut = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)),
                                       this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, i]() {
            m_tab_widget->setCurrentIndex(i);
        });
    }

    // Ctrl+0: switches to tab index 9 (10th tab) -- browser convention
    if (tabCount > 9) {
        auto* tab10 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
        tab10->setContext(Qt::ApplicationShortcut);
        connect(tab10, &QShortcut::activated, this, [this]() { m_tab_widget->setCurrentIndex(9); });
    }

    // Ctrl+Shift+1..5: switches to tab index 10..14 (tabs beyond 10)
    for (int i = 0; i < qMin(tabCount - 10, 5); ++i) {
        const int tabIdx = 10 + i;
        if (tabIdx >= tabCount) {
            break;
        }
        auto* shortcut = new QShortcut(
            QKeySequence(Qt::CTRL | Qt::SHIFT | static_cast<Qt::Key>(Qt::Key_1 + i)), this);
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
        int prev = (m_tab_widget->currentIndex() - 1 + m_tab_widget->count()) %
                   m_tab_widget->count();
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

void MainWindow::loadWindowState() {
    Q_ASSERT(m_tab_widget);
    auto& config = ConfigManager::instance();

    if (config.getRestoreWindowGeometry()) {
        restoreGeometry(config.getWindowGeometry());
        restoreState(config.getWindowState());
    }

    // Always start on Quick Actions tab (index 0)
    m_tab_widget->setCurrentIndex(0);
}

void MainWindow::saveWindowState() {
    auto& config = ConfigManager::instance();
    config.setWindowGeometry(saveGeometry());
    config.setWindowState(saveState());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    Q_ASSERT(event);
    event->accept();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    Q_ASSERT(event);
    QMainWindow::moveEvent(event);
    if (m_logWindow) {
        m_logWindow->repositionIfAnchored();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    Q_ASSERT(event);
    QMainWindow::resizeEvent(event);
    if (m_logWindow) {
        m_logWindow->repositionIfAnchored();
    }
    // Scroll buttons may appear/disappear on resize
    QTimer::singleShot(0, this, &MainWindow::applyTabBarChevrons);
}

void MainWindow::applyTabBarChevrons() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(m_tab_widget->tabBar());
    if (!m_tab_widget) {
        return;
    }

    const auto buttons = m_tab_widget->tabBar()->findChildren<QToolButton*>();
    for (auto* btn : buttons) {
        btn->setArrowType(Qt::NoArrow);
        btn->setIcon(QIcon());
        // Use unicode chevron text -- inherits the theme's white-on-blue styling
        if (btn == buttons.value(0)) {
            btn->setText(QStringLiteral("\u276E"));  // <
        } else {
            btn->setText(QStringLiteral("\u276F"));  // >
        }
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_tab_widget->tabBar() && event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const int raw_y = wheel->angleDelta().y();
        const int raw_x = wheel->angleDelta().x();
        const int delta = raw_y != 0 ? raw_y : raw_x;
        if (delta == 0) {
            return QMainWindow::eventFilter(obj, event);
        }
        const int current = m_tab_widget->currentIndex();
        const int count = m_tab_widget->count();
        // Scroll up/right = previous tab, scroll down/left = next tab
        const int next = delta > 0 ? qMax(0, current - 1) : qMin(count - 1, current + 1);
        if (next != current) {
            m_tab_widget->setCurrentIndex(next);
        }
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
}

}  // namespace sak
