// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main_window.cpp
/// @brief Implements the main application window with tabbed panel navigation

#include "sak/main_window.h"

#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
#include "sak/ai_assistant_panel.h"
#endif
#include "sak/advanced_search_panel.h"
#include "sak/advanced_uninstall_panel.h"
#include "sak/app_installation_panel.h"
#include "sak/config_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/diagnostic_benchmark_panel.h"
#include "sak/elevation_manager.h"
#include "sak/email_inspector_panel.h"
#include "sak/image_flasher_panel.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/network_diagnostic_panel.h"
#include "sak/organizer_panel.h"
#include "sak/ost_converter_widget.h"
#include "sak/partition_manager_panel.h"
#include "sak/rich_text_safety.h"
#include "sak/style_constants.h"
#include "sak/user_migration_panel.h"
#include "sak/version.h"
#include "sak/vulnerability_panel.h"
#include "sak/widget_helpers.h"
#include "sak/wifi_manager_panel.h"
#include "sak/windows11_theme.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QMoveEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSysInfo>
#include <QTabBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <array>

namespace sak {

namespace {

constexpr int kStatusIconSize = ui::kUiIconSmall;
constexpr int kDefaultPanelStatusTimeoutMs = kTimerStatusDefaultMs;
constexpr int kDirectTabShortcutCount = 9;
// Per-tab cap on the retained log backing store. Untrusted file/network/AI output flows
// into these lines, so the store is bounded and evicts the oldest rather than growing for
// the whole session.
constexpr int kMaxPanelLogLines = 5000;
constexpr int kTenthTabIndex = 9;
constexpr int kTabShortcutBaseOffset = 10;
constexpr int kShiftTabShortcutCount = 5;
constexpr auto kTabScrollButtonObjectName = "sakTabScrollButton";
constexpr auto kTabScrollDirectionProperty = "sakTabScrollDirection";
constexpr auto kTabScrollDirectionLeft = "left";
constexpr auto kTabScrollDirectionRight = "right";

QBoxLayout* findBoxLayoutContainingWidget(QLayout* layout, QWidget* target, int& item_index) {
    if ((layout == nullptr) || (target == nullptr)) {
        return nullptr;
    }

    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item == nullptr) {
            continue;
        }
        if (item->widget() == target) {
            item_index = index;
            return dynamic_cast<QBoxLayout*>(layout);
        }
        if (auto* child_layout = item->layout()) {
            if (auto* found = findBoxLayoutContainingWidget(child_layout, target, item_index)) {
                return found;
            }
        }
    }
    return nullptr;
}

constexpr char kAboutTabHtml[] = R"SAKABOUT(
<h2>Swiss Army Knife (S.A.K.) Utility</h2>
<div class="subtitle">A portable Windows toolkit for PC technicians, IT pros, and sysadmins.<br/>
Built with modern C++23 and Qt 6 for Windows 10/11 x64.</div>

<div class="section">
    <div class="section-title">AI Assistant</div>
    <ul>
        <li><b>Codex-style Technician Chat</b> &mdash; Prompt/result transcript, session picker, rename/new chat controls, prompt history, context chips, and expandable run details</li>
        <li><b>OpenAI Integration</b> &mdash; User-supplied API key with encrypted app-local storage, model selection, Responses API conversation state, web citations, and token usage tracking</li>
        <li><b>Context &amp; Instructions</b> &mdash; Attach screenshots, documents, and Markdown instruction files to the active session without cross-session artifact pollution</li>
        <li><b>PC Actions</b> &mdash; PowerShell, cmd.exe, direct process launch, screenshots, HTTPS downloads, package-manager actions, and offline downloader actions with approval gates and cancellation</li>
        <li><b>Multi-Agent Workflows</b> &mdash; Overseer-led workflow execution with specialized subagents, shared session memory, progress tracking, recovery policy, verification, cleanup, and human handoff</li>
        <li><b>Technician Workflow Library</b> &mdash; Health checks, drive diagnostics, Windows Update repair, network repair, BSOD triage, printer troubleshooting, startup triage, malware removal, cleanup, bloatware/adware removal, offline installers, deployment bundles, clean uninstall, and service reports</li>
        <li><b>Reports &amp; Artifacts</b> &mdash; Manual HTML/Markdown/text reports generated from the actual session transcript, findings, actions, evidence, verification, and remaining recommendations</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Migration &amp; Backup</div>
    <ul>
        <li><b>User Profile Backup &amp; Restore</b> &mdash; Step-by-step wizards with smart filtering, per-user customization, AES-256 encryption, and NTFS permission handling</li>
        <li><b>Quick Tools</b> &mdash; One-click Screenshot Settings capture and BitLocker Key Backup integrated directly into the panel</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Diagnostics &amp; Benchmarking</div>
    <ul>
        <li><b>Hardware Inventory</b> &mdash; CPU, memory, storage, GPU, and OS details</li>
        <li><b>SMART Disk Health</b> &mdash; Drive health, temperature, power-on hours, and attribute monitoring via bundled smartmontools</li>
        <li><b>Benchmarks</b> &mdash; CPU (single/multi-thread), disk (sequential &amp; random I/O), memory (bandwidth &amp; latency)</li>
        <li><b>Stress Testing</b> &mdash; CPU, memory, and disk stress with real-time thermal monitoring and configurable auto-abort</li>
        <li><b>System Maintenance</b> &mdash; Optimize Power Settings, Verify System Files, Check Disk Errors, and Generate System Report</li>
        <li><b>Report Export</b> &mdash; HTML, JSON, and CSV reports</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Partition Manager</div>
    <ul>
        <li><b>Disk Workspace</b> &mdash; Lazy storage inventory, proportional disk map, partition table, right-click actions, pending queue, dry run, and before/after Apply review</li>
        <li><b>Queued Operations</b> &mdash; Create, delete, format, resize, move start, merge, split, clone, image, restore, label, drive-letter, type ID, active flag, hide/unhide, wipe, initialize, delete-all, and boot repair flows</li>
        <li><b>Commercial-Parity Utilities</b> &mdash; Quick Partition, Extend Partition Wizard, Allocate Free Space, Allocate Free Space To, Space Analyzer, Disk Benchmark, BitLocker status, Disk Defrag, SSD Secure Erase, Data Recovery, OS migration, and Make Bootable Media</li>
        <li><b>Destructive Safety</b> &mdash; Off-volume backup, typed confirmations, system/boot/removable/media guards, script preview, elevated execution, restore verification, SHA-256 manifest comparison, and repair scans where mutation requires rebuild</li>
        <li><b>Certification Evidence</b> &mdash; Disposable-VHD proof and imported external VM/hardware/lab evidence are checked by release-readiness harnesses before hardware-certified release claims are accepted</li>
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
        <li><b>App Installation</b> &mdash; Scan installed apps, match to Chocolatey packages, export/import, bulk-install on a new PC, and offline deployment with direct installer downloads</li>
        <li><b>Advanced Uninstall</b> &mdash; Deep application removal with leftover scanning, registry snapshot diffs, recycle bin support, and locked-file reboot scheduling</li>
        <li><b>Vulnerability Scanner</b> &mdash; Check installed apps and packages against CISA KEV, NVD, GitHub Security Advisories, and OSV with patch guidance and CSV/JSON export</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Imaging</div>
    <ul>
        <li><b>Image Flasher</b> &mdash; Flash ISOs/IMGs to USB with streaming decompression and system-drive protection</li>
        <li><b>Windows ISO Download</b> &mdash; Build Windows ISOs from Microsoft UUP payloads through the UUP Dump API and bundled UUP-to-ISO tooling</li>
        <li><b>Linux ISO Download</b> &mdash; Built-in distro catalog (Ubuntu, Fedora, Debian, Arch, Mint, Kali, SystemRescue, Clonezilla, and more)</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Network Management</div>
    <ul>
        <li><b>Network Diagnostics</b> &mdash; Ping, traceroute, MTR, DNS lookup, port scan, bandwidth test, WiFi analyzer, active connections, firewall auditor, and network share browser</li>
        <li><b>Network Adapters</b> &mdash; Adapter inspector with ethernet configuration backup and restore across machines, plus one-click Reset Network Settings</li>
        <li><b>WiFi Manager</b> &mdash; QR code generation, network scanning, bulk export, and connection scripts</li>
    </ul>
</div>

<div class="section">
    <div class="section-title">Email &amp; Data Forensics</div>
    <ul>
        <li><b>PST/OST/MBOX Email Tools</b> &mdash; Offline forensic inspection of Outlook and Thunderbird email archives without client installation</li>
        <li><b>Email Search &amp; Export</b> &mdash; Full-text search across thousands of items with checkbox export to HTML, TXT, EML, PDF, ICS, VCF, and CSV formats</li>
        <li><b>Contacts Browser</b> &mdash; Searchable address book with sortable columns and export to VCF or CSV</li>
        <li><b>Calendar Viewer</b> &mdash; Month, week, and day views with event details and export to ICS or CSV</li>
        <li><b>Attachments Browser</b> &mdash; Scan all emails for attachments with type filtering, search, and batch extraction</li>
        <li><b>Email Profile Manager</b> &mdash; Backup and restore profiles for Outlook, Thunderbird, and Windows Mail</li>
        <li><b>Orphaned File Discovery</b> &mdash; Scan drives for orphaned PST, OST, and MBOX files not linked to active profiles</li>
        <li><b>OST/PST Converter</b> &mdash; Multi-threaded bulk conversion to EML, MBOX, HTML, and PDF with deleted item recovery</li>
    </ul>
</div>

<div class="footer">
    Portable ZIP &mdash; no installer required. AI sessions and credentials stay app-local.<br/>
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
smartmontools (GPLv2), aria2 (GPLv2), UUPMediaCreator (MIT), iPerf3 (BSD 3-Clause),
qrcodegen (MIT), and additional open-source libraries. See the Credits tab
for the full list.</p>
)SAKLICENSE";

constexpr char kCreditsTabHtml[] = R"SAKCREDITS(
<h3>Development</h3>
<p><b>Lead Developer:</b> Randy Northrup</p>

<h3>Third-Party Components</h3>

<div class="dep">
    <b><a href="https://www.qt.io/">Qt Framework 6.5+</a></b> &mdash; LGPL v3<br/>
    <span class="desc">GUI framework, threading, networking, resources, and application infrastructure</span>
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
    <b><a href="https://iperf.fr/">iPerf3</a></b> &mdash; BSD 3-Clause<br/>
    <span class="desc">LAN bandwidth testing (bundled iperf3.exe)</span>
</div>
<div class="dep">
    <b><a href="https://icons8.com/">Icons8</a></b> &mdash; Icons8 Free License<br/>
    <span class="desc">Windows 11 Filled (Fluent) SVG icons with attribution</span>
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
    <span class="desc">AES-256 encryption, PBKDF2, and SHA-256 through Windows cryptography APIs</span>
</div>

<h3>Special Thanks</h3>
<p>To the C++ and Qt communities for their excellent documentation and support.</p>
<p>To Microsoft for Windows API documentation, PowerShell, Windows SDK, and ADK tools.</p>
)SAKCREDITS";

// Tab tooltips carry no hardcoded shortcut hints: the Ctrl+N hint depends on
// the tab's actual index (which shifts with SAK_ENABLE_AI_ASSISTANT), so it
// is appended per-index by UpdateTabShortcutHints below.
constexpr char kTooltipUserMigration[] = "Backup and restore user profiles";
constexpr char kTooltipOrganizer[] = "Organize files, find duplicates, and advanced search";
constexpr char kTooltipAppManagement[] = "Install, uninstall, and manage applications";
constexpr char kTooltipImageFlasher[] = "Flash ISO images to USB drives";
constexpr char kTooltipDiagnostics[] = "System diagnostics, benchmarks, and stress tests";
constexpr char kTooltipNetworkManagement[] =
    "Network diagnostics, WiFi management, and connectivity tools";
constexpr char kTooltipEmailTool[] =
    "Inspect PST, OST, and MBOX email files -- search, export, and manage profiles";

#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
constexpr char kTooltipAiAssistant[] =
    "AI-assisted research, reports, and full-access PC repair workflows";
#endif

constexpr int kTabIconSize = 20;

bool isAccessibilityAuditMode() {
    const auto* app = QCoreApplication::instance();
    return (app != nullptr) && app->property("sakAccessibilityAudit").toBool();
}

bool isStartupSmokeMode() {
    const auto* app = QCoreApplication::instance();
    return (app != nullptr) && app->property("sakStartupSmokeTest").toBool();
}

void addTabWithTooltip(QTabWidget* tab_widget,
                       QWidget* panel,
                       const char* tab_title,
                       const char* tooltip,
                       const char* icon_path = nullptr) {
    Q_ASSERT(tab_widget);
    Q_ASSERT(panel);
    Q_ASSERT(tab_title);
    Q_ASSERT(tooltip);

    const QIcon icon = (icon_path != nullptr) ? QIcon(QString::fromUtf8(icon_path)) : QIcon();
    const int idx = icon.isNull() ? tab_widget->addTab(panel, QString::fromUtf8(tab_title))
                                  : tab_widget->addTab(panel, icon, QString::fromUtf8(tab_title));
    if (!icon.isNull()) {
        tab_widget->setIconSize(QSize(kTabIconSize, kTabIconSize));
    }
    tab_widget->setTabToolTip(idx, QString::fromUtf8(tooltip));
    // Base tooltip without the shortcut hint; UpdateTabShortcutHints rebuilds
    // the visible tooltip from this whenever tab positions are final.
    tab_widget->tabBar()->setTabData(idx, QString::fromUtf8(tooltip));

    // Set accessible name on the panel widget so screen readers
    // identify each tab's content area
    const QString title = QString::fromUtf8(tab_title);
    panel->setAccessibleName(title + QStringLiteral(" panel"));
    panel->setAccessibleDescription(QString::fromUtf8(tooltip));

    // Also set accessible text on the tab bar tab via tab widget
    tab_widget->setTabWhatsThis(idx, QString::fromUtf8(tooltip));
}

/// Shortcut hint for a tab index, mirroring createKeyboardShortcuts exactly:
/// Ctrl+1..9 select indices 0..8, Ctrl+0 selects index 9, and Ctrl+Shift+1..5
/// select indices 10..14.
QString tabShortcutHint(int index) {
    if (index < kDirectTabShortcutCount) {
        return QStringLiteral(" (Ctrl+%1)").arg(index + 1);
    }
    if (index == kTenthTabIndex) {
        return QStringLiteral(" (Ctrl+0)");
    }
    if (index >= kTabShortcutBaseOffset &&
        index < kTabShortcutBaseOffset + kShiftTabShortcutCount) {
        return QStringLiteral(" (Ctrl+Shift+%1)").arg(index - kTabShortcutBaseOffset + 1);
    }
    return {};
}

/// Rebuild every tab's tooltip as its stored base text plus the shortcut hint
/// for the index the tab actually occupies, so the advertised Ctrl+N always
/// selects the tab it is written on regardless of build configuration.
void updateTabShortcutHints(QTabWidget* tab_widget) {
    Q_ASSERT(tab_widget);
    const QTabBar* bar = tab_widget->tabBar();
    for (int idx = 0; idx < tab_widget->count(); ++idx) {
        const QString base = bar->tabData(idx).toString();
        if (base.isEmpty()) {
            continue;
        }
        const QString tooltip = base + tabShortcutHint(idx);
        tab_widget->setTabToolTip(idx, tooltip);
        tab_widget->setTabWhatsThis(idx, tooltip);
        if (QWidget* panel = tab_widget->widget(idx)) {
            panel->setAccessibleDescription(tooltip);
        }
    }
}

void applyHtmlBrowserTheme(QTextBrowser* browser) {
    if (browser == nullptr) {
        return;
    }
    browser->setStyleSheet(ui::textBrowserSurfaceStyle(ui::kColorBgWhite, ui::kColorBorderDefault));
    browser->document()->setDefaultStyleSheet(ui::htmlBrowserDocumentStyleSheet());
    const QString html = browser->property("sakHtmlContent").toString();
    if (!html.isEmpty()) {
        browser->setHtml(html);
    }
}

void applyThemedHtmlBrowsers(QWidget* root) {
    if (root == nullptr) {
        return;
    }
    const auto browsers = root->findChildren<QTextBrowser*>();
    for (QTextBrowser* browser : browsers) {
        if ((browser != nullptr) && browser->property("sakThemedHtmlBrowser").toBool()) {
            applyHtmlBrowserTheme(browser);
        }
    }
}

QTextBrowser* createHtmlBrowser(QWidget* parent, const char* html, const QString& accessible_name) {
    Q_ASSERT(parent);
    Q_ASSERT(html);
    Q_ASSERT(!accessible_name.isEmpty());

    auto* browser = new QTextBrowser(parent);
    browser->setOpenExternalLinks(true);
    browser->setAccessibleName(accessible_name);
    browser->setProperty("sakThemedHtmlBrowser", true);
    browser->setProperty("sakHtmlContent", QString::fromUtf8(html));
    applyHtmlBrowserTheme(browser);
    return browser;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    QElapsedTimer startup_timer;
    startup_timer.start();
    setupUi();
    logInfo("MainWindow startup: setupUi took {} ms", startup_timer.restart());
    if (!isAccessibilityAuditMode()) {
        loadWindowState();
        logInfo("MainWindow startup: loadWindowState took {} ms", startup_timer.elapsed());
    }
}

MainWindow::~MainWindow() {
    // Must be first: stops the tab-change status-bar handlers from running while
    // the window is being destroyed (they would dereference freed tool panels).
    m_shutting_down = true;
    if (!isAccessibilityAuditMode()) {
        saveWindowState();
    }
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

    // Create UI elements. Phase timing goes to the log so a slow launch on a
    // real desktop can be attributed to a specific phase after the fact.
    QElapsedTimer phase_timer;
    phase_timer.start();
    createStatusBar();
    logInfo("MainWindow startup: status bar took {} ms", phase_timer.restart());

    // Create shared log window BEFORE panels so connectLog() can reference it
    m_logWindow = new DetachableLogWindow(tr("S.A.K. Log"), this);
    logInfo("MainWindow startup: log window took {} ms", phase_timer.restart());

    createPanels();
    logInfo("MainWindow startup: panels took {} ms", phase_timer.restart());
    createKeyboardShortcuts();

    if (!isAccessibilityAuditMode()) {
        // Apply chevron icons to the tab-bar scroll buttons.
        // Qt creates the scroll QToolButtons lazily when tabs overflow, so we
        // re-apply after layout and on tab changes to cover all cases.
        QTimer::singleShot(0, this, &MainWindow::applyTabBarChevrons);
        connect(m_tab_widget, &QTabWidget::currentChanged, this, [this](int) {
            QTimer::singleShot(0, this, &MainWindow::applyTabBarChevrons);
            updateVulnerabilityStatusBarVisibility();
#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
            updateAiStatusBarVisibility();
#endif
        });

        // Enable mouse-wheel tab switching on the tab bar
        m_tab_widget->tabBar()->installEventFilter(this);
    }

    updateStatus("Ready", 0);
}

void MainWindow::createStatusBar() {
    // First point in startup that needs the font database; with the async
    // warmup from main() this normally reads back instantly, and the log
    // shows how long the GUI thread still had to wait when it did not.
    QElapsedTimer font_timer;
    font_timer.start();
    const auto font_family_count = QFontDatabase::families().size();
    logInfo("MainWindow startup: font database ready ({} families) after {} ms",
            font_family_count,
            font_timer.elapsed());

    // Persistent status label. Every panel relays status text through it, and that text embeds
    // file paths, program names, adapter names and AI output, so it renders verbatim.
    m_status_label = sak::plainTextLabel("Ready", this);
    m_status_label->setContentsMargins(
        sak::ui::kMarginTight, sak::ui::kMarginNone, sak::ui::kMarginTight, sak::ui::kMarginNone);
    statusBar()->addWidget(m_status_label, 1);

    m_vulnerability_summary_label = new QLabel(this);
    m_vulnerability_summary_label->setContentsMargins(sak::ui::kSpacingDefault,
                                                      sak::ui::kMarginNone,
                                                      sak::ui::kSpacingDefault,
                                                      sak::ui::kMarginNone);
    m_vulnerability_summary_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_vulnerability_summary_label->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(ui::kFontWeightSemibold, sak::ui::kColorTextSecondary));
    m_vulnerability_summary_label->setVisible(false);

#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
    // Mirrors the AI panel's run details (model/tool text), so it renders verbatim too.
    m_ai_status_label = sak::plainTextLabel(QString(), this);
    m_ai_status_label->setContentsMargins(sak::ui::kSpacingDefault,
                                          sak::ui::kMarginNone,
                                          sak::ui::kSpacingDefault,
                                          sak::ui::kMarginNone);
    m_ai_status_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_ai_status_label->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(ui::kFontWeightSemibold, sak::ui::kColorTextSecondary));
    m_ai_status_label->setVisible(false);
#endif

    // Progress bar (hidden by default, fixed size to prevent resizing)
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setFixedWidth(sak::kProgressBarMaxW);
    m_progress_bar->setFixedHeight(sak::kProgressBarMaxH);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setVisible(false);
    statusBar()->addPermanentWidget(m_progress_bar);

#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
    statusBar()->addPermanentWidget(m_ai_status_label);
#endif
    statusBar()->addPermanentWidget(m_vulnerability_summary_label);

    createElevationStatusWidgets();
}

void MainWindow::createElevationStatusWidgets() {
    // Elevation status indicator
    m_elevation_label = new QWidget(this);
    auto* elev_layout = new QHBoxLayout(m_elevation_label);
    elev_layout->setContentsMargins(
        sak::ui::kMarginTight, sak::ui::kMarginNone, sak::ui::kMarginTight, sak::ui::kMarginNone);
    elev_layout->setSpacing(sak::ui::kSpacingTight);
    auto* elev_icon = new QLabel(m_elevation_label);
    auto* elev_text = new QLabel(m_elevation_label);
    const bool is_elevated = ElevationManager::isElevated();
    if (is_elevated) {
        elev_icon->setPixmap(QIcon(QStringLiteral(":/icons/icons/icons8-keyhole-shield.svg"))
                                 .pixmap(kStatusIconSize, kStatusIconSize));
        elev_text->setText(tr("Administrator"));
        m_elevation_label->setToolTip(tr("Running with administrator privileges"));
    } else {
        elev_icon->setPixmap(QIcon(QStringLiteral(":/icons/icons/icons8-lock.svg"))
                                 .pixmap(kStatusIconSize, kStatusIconSize));
        elev_text->setText(tr("Standard"));
        m_elevation_label->setToolTip(
            tr("Running as standard user. Some features will prompt "
               "for elevation."));
    }
    elev_layout->addWidget(elev_icon);
    elev_layout->addWidget(elev_text);
    statusBar()->addPermanentWidget(m_elevation_label);

    // "Restart as Administrator" button (only when not already elevated)
    if (!is_elevated) {
        auto* elevate_btn = new QPushButton(this);
        elevate_btn->setText(tr("Run as Admin"));
        elevate_btn->setToolTip(tr("Restart S.A.K. Utility with administrator privileges"));
        elevate_btn->setAccessibleName(tr("Run as Administrator"));
        elevate_btn->setFlat(true);
        elevate_btn->setCursor(Qt::PointingHandCursor);
        elevate_btn->setStyleSheet(sak::ui::compactLinkButtonStyle());
        connect(elevate_btn, &QPushButton::clicked, this, [this]() {
            auto result = ElevationManager::restartElevated();
            if (result) {
                QCoreApplication::quit();
            } else {
                sak::logError("Failed to restart elevated from status bar: {}",
                              to_string(result.error()));
                sak::showCriticalLogged(this,
                                        tr("Elevation Failed"),
                                        tr("Failed to restart with administrator privileges.\n\n"
                                           "Try right-clicking the application and selecting "
                                           "\"Run as administrator\"."));
            }
        });
        statusBar()->addPermanentWidget(elevate_btn);
    }
}

void MainWindow::createPanels() {
    QElapsedTimer phase_timer;
    phase_timer.start();
    registerLazyToolTabs();
    logInfo("MainWindow startup: lazy tab placeholders took {} ms", phase_timer.restart());
    createHelpPanel();
    logInfo("MainWindow startup: help panel took {} ms", phase_timer.restart());
    createAboutPanel();
    logInfo("MainWindow startup: about panel took {} ms", phase_timer.restart());
    // Build each tool panel only when its tab is first activated. Connected
    // before setupLogRouting so the panel exists before onTabChanged runs.
    connect(m_tab_widget, &QTabWidget::currentChanged, this, [this](int index) {
        // currentChanged also fires while tabs are torn down during ~MainWindow
        // (panels removed/destroyed). Building a panel then would assign a member
        // unique_ptr mid-teardown; skip lazy materialization once shutting down.
        if (m_shutting_down) {
            return;
        }
        materializeTab(index);
    });
    setupLogRouting();
    updateTabShortcutHints(m_tab_widget);
    if (isAccessibilityAuditMode() || isStartupSmokeMode()) {
        // The accessibility scan needs the full widget tree, and the startup
        // smoke gate must catch a crash in ANY panel constructor - lazy tabs
        // would otherwise leave every tool panel unbuilt in both modes.
        for (int slot = 0; slot < static_cast<int>(m_lazyTabs.size()); ++slot) {
            materializeTab(slot);
        }
    }
}

void MainWindow::registerLazyToolTabs() {
    Q_ASSERT(m_tab_widget);
#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
    addLazyPlaceholder("AI Assistant", kTooltipAiAssistant, ":/icons/icons/panel_ai.svg");
    m_lazyTabs.push_back(
        {.build = [this] { createAiAssistantPanelTab(); }, .wire = [this] { wireAiPanel(); }});
#endif
    addLazyPlaceholder("Backup and Restore",
                       kTooltipUserMigration,
                       ":/icons/icons/panel_backup_restore.svg");
    m_lazyTabs.push_back(
        {.build = [this] { createBackupRestorePanel(); }, .wire = [this] { wireBackupPanel(); }});

    addLazyPlaceholder("File Management", kTooltipOrganizer, ":/icons/icons/panel_organizer.svg");
    m_lazyTabs.push_back({.build = [this] { createFileManagementPanel(); },
                          .wire = [this] { wireFileManagementPanels(); }});

    addLazyPlaceholder("Partition Manager",
                       "Manage disks, partitions, SMART status, cloning, migration, boot repair, "
                       "SSD optimization, and wipe operations",
                       ":/icons/icons/icons8-pm-disk.svg");
    m_lazyTabs.push_back({.build = [this] { createPartitionManagerPanel(); },
                          .wire = [this] { wirePartitionPanel(); }});

    m_slotImageFlasher = static_cast<int>(m_lazyTabs.size());
    addLazyPlaceholder("Image Flasher",
                       kTooltipImageFlasher,
                       ":/icons/icons/panel_image_flasher.svg");
    m_lazyTabs.push_back({.build = [this] { createImageFlasherPanel(); },
                          .wire = [this] { wireImageFlasherPanel(); }});

    m_slotDiagnostic = static_cast<int>(m_lazyTabs.size());
    addLazyPlaceholder("Benchmark and Diagnostics",
                       kTooltipDiagnostics,
                       ":/icons/icons/panel_diagnostic.svg");
    m_lazyTabs.push_back(
        {.build = [this] { createDiagnosticPanel(); }, .wire = [this] { wireDiagnosticPanel(); }});

    addLazyPlaceholder("Email Tools", kTooltipEmailTool, ":/icons/icons/panel_email.svg");
    m_lazyTabs.push_back(
        {.build = [this] { createEmailToolsPanel(); }, .wire = [this] { wireEmailPanels(); }});

    addLazyPlaceholder("Application Management",
                       kTooltipAppManagement,
                       ":/icons/icons/panel_app_install.svg");
    m_lazyTabs.push_back({.build = [this] { createAppManagementPanel(); },
                          .wire = [this] { wireAppManagementPanels(); }});

    addLazyPlaceholder("Network Management",
                       kTooltipNetworkManagement,
                       ":/icons/icons/panel_network.svg");
    m_lazyTabs.push_back({.build = [this] { createNetworkManagementPanel(); },
                          .wire = [this] { wireNetworkPanels(); }});
}

void MainWindow::addLazyPlaceholder(const char* title, const char* tooltip, const char* icon_path) {
    Q_ASSERT(m_tab_widget);
    auto* placeholder = new QWidget(this);
    addTabWithTooltip(m_tab_widget, placeholder, title, tooltip, icon_path);
}

void MainWindow::materializeTab(int slot) {
    if (slot < 0 || slot >= static_cast<int>(m_lazyTabs.size())) {
        return;
    }
    auto& lazy = m_lazyTabs[slot];
    if (lazy.built) {
        return;
    }
    lazy.built = true;

    QWidget* placeholder = m_tab_widget->widget(slot);
    QElapsedTimer build_timer;
    build_timer.start();
    lazy.build();  // Constructs the real panel and appends its tab at the end.
    logInfo("MainWindow: tab {} built in {} ms",
            m_tab_widget->tabText(m_tab_widget->count() - 1).toStdString(),
            build_timer.restart());

    // Move the freshly built tab into the placeholder's slot and drop the
    // placeholder. Signals are blocked so the restructure does not emit a storm
    // of currentChanged() while we are already inside one.
    {
        const QSignalBlocker blocker(m_tab_widget);
        const int appended = m_tab_widget->count() - 1;
        m_tab_widget->tabBar()->moveTab(appended, slot);  // real -> slot; placeholder -> slot + 1
        m_tab_widget->removeTab(slot + 1);
        m_tab_widget->setCurrentIndex(slot);
    }
    if (placeholder != nullptr) {
        placeholder->deleteLater();
    }

    lazy.wire();  // Panel now occupies its final slot; safe to wire status/logs.
    // The rebuilt tab re-set its base tooltip; re-append the shortcut hint.
    updateTabShortcutHints(m_tab_widget);
}

#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
void MainWindow::createAiAssistantPanelTab() {
    m_ai_assistant_panel = std::make_unique<AiAssistantPanel>(this);
    addTabWithTooltip(m_tab_widget,
                      m_ai_assistant_panel.get(),
                      "AI Assistant",
                      kTooltipAiAssistant,
                      ":/icons/icons/panel_ai.svg");
}
#endif

void MainWindow::createBackupRestorePanel() {
    logInfo("MainWindow: creating Backup and Restore panel");
    m_user_migration_panel = std::make_unique<UserMigrationPanel>(this);
    addTabWithTooltip(m_tab_widget,
                      m_user_migration_panel.get(),
                      "Backup and Restore",
                      kTooltipUserMigration,
                      ":/icons/icons/panel_backup_restore.svg");
    logInfo("MainWindow: Backup and Restore panel initialized");
}

void MainWindow::createFileManagementPanel() {
    logInfo("MainWindow: creating File Management panels");
    m_organizer_panel = std::make_unique<OrganizerPanel>(this);
    m_advanced_search_panel = std::make_unique<AdvancedSearchPanel>(this);
    m_organizer_panel->tabWidget()->addTab(m_advanced_search_panel.get(), tr("Advanced Search"));
    addTabWithTooltip(m_tab_widget,
                      m_organizer_panel.get(),
                      "File Management",
                      kTooltipOrganizer,
                      ":/icons/icons/panel_organizer.svg");
    logInfo("MainWindow: File Management panels initialized");
}

void MainWindow::createPartitionManagerPanel() {
    logInfo("MainWindow: creating Partition Manager panel");
    m_partition_manager_panel = std::make_unique<PartitionManagerPanel>(this);
    addTabWithTooltip(m_tab_widget,
                      m_partition_manager_panel.get(),
                      "Partition Manager",
                      "Manage disks, partitions, SMART status, cloning, migration, boot repair, "
                      "SSD optimization, and wipe operations",
                      ":/icons/icons/icons8-pm-disk.svg");
    logInfo("MainWindow: Partition Manager panel initialized");
}

void MainWindow::createImageFlasherPanel() {
    logInfo("MainWindow: creating Image Flasher panel");
    m_image_flasher_panel = std::make_unique<ImageFlasherPanel>(this);
    addTabWithTooltip(m_tab_widget,
                      m_image_flasher_panel.get(),
                      "Image Flasher",
                      kTooltipImageFlasher,
                      ":/icons/icons/panel_image_flasher.svg");
    logInfo("MainWindow: Image Flasher panel initialized");
}

void MainWindow::createDiagnosticPanel() {
    logInfo("MainWindow: creating Benchmark and Diagnostics panel");
    m_diagnostic_benchmark_panel = std::make_unique<DiagnosticBenchmarkPanel>(this);
    addTabWithTooltip(m_tab_widget,
                      m_diagnostic_benchmark_panel.get(),
                      "Benchmark and Diagnostics",
                      kTooltipDiagnostics,
                      ":/icons/icons/panel_diagnostic.svg");
    logInfo("MainWindow: Benchmark and Diagnostics panel initialized");
}

void MainWindow::createEmailToolsPanel() {
    logInfo("MainWindow: creating Email Tools panels");
    m_email_inspector_panel = std::make_unique<EmailInspectorPanel>(this);
    m_ost_converter_widget = std::make_unique<OstConverterWidget>(this);

    auto* email_wrapper = new QWidget(this);
    auto* email_layout = new QVBoxLayout(email_wrapper);
    email_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    email_layout->setSpacing(ui::kSpacingDefault);

    auto email_hdr = sak::createDynamicPanelHeader(
        email_wrapper,
        QStringLiteral(":/icons/icons/panel_email.svg"),
        tr("Email Tools"),
        tr("Offline email forensics, data extraction, and OST/PST conversion"),
        email_layout);

    auto* email_tabs = new QTabWidget(email_wrapper);
    email_tabs->setAccessibleName(tr("Email tools tabs"));
    email_tabs->addTab(m_email_inspector_panel.get(), tr("Email Inspector"));
    email_tabs->addTab(m_ost_converter_widget.get(), tr("OST Converter"));
    email_layout->addWidget(email_tabs, 1);

    connect(email_tabs, &QTabWidget::currentChanged, this, [email_hdr](int index) {
        struct TabMeta {
            const char* icon;
            const char* title;
            const char* subtitle;
        };
        static constexpr std::array<TabMeta, 2> kTabs = {{
            {.icon = ":/icons/icons/panel_email.svg",
             .title = "Email Inspector",
             .subtitle = "Offline email forensics and data extraction "
                         "\xe2\x80\x94 inspect PST, OST, and MBOX files"},
            {.icon = ":/icons/icons/panel_email.svg",
             .title = "OST Converter",
             .subtitle = "Bulk OST/PST file conversion to EML, MSG, MBOX, and more"},
        }};
        if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
            const auto& m = kTabs[index];
            sak::updatePanelHeader(email_hdr,
                                   QString::fromUtf8(m.icon),
                                   QCoreApplication::translate("MainWindow", m.title),
                                   QCoreApplication::translate("MainWindow", m.subtitle));
        }
    });

    connect(m_ost_converter_widget.get(),
            &OstConverterWidget::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_ost_converter_widget.get(),
            &OstConverterWidget::progressUpdate,
            this,
            &MainWindow::updateProgress);

    addTabWithTooltip(m_tab_widget,
                      email_wrapper,
                      "Email Tools",
                      kTooltipEmailTool,
                      ":/icons/icons/panel_email.svg");
    logInfo("MainWindow: Email Tools panels initialized");
}

void MainWindow::createAppManagementPanel() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(!m_app_installation_panel);
    createAppManagementChildPanels();

    logInfo("MainWindow: creating Application Management wrapper");
    auto* app_mgmt_wrapper = new QWidget(this);
    auto* app_mgmt_layout = new QVBoxLayout(app_mgmt_wrapper);
    app_mgmt_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    app_mgmt_layout->setSpacing(ui::kSpacingDefault);

    auto app_hdr = sak::createDynamicPanelHeader(
        app_mgmt_wrapper,
        QStringLiteral(":/icons/icons/panel_app_install.svg"),
        tr("App Installation"),
        tr("Search, queue, and batch-install applications via Chocolatey"),
        app_mgmt_layout);

    auto* app_tabs = new QTabWidget(app_mgmt_wrapper);
    app_tabs->setAccessibleName(tr("Application management tabs"));
    m_application_tabs = app_tabs;
    app_tabs->addTab(m_app_installation_panel.get(), tr("App Installation"));
    app_tabs->addTab(m_advanced_uninstall_panel.get(), tr("Advanced Uninstall"));
    app_tabs->addTab(m_vulnerability_panel.get(), tr("Vulnerability Scanner"));
    app_mgmt_layout->addWidget(app_tabs, 1);

    connect(app_tabs, &QTabWidget::currentChanged, this, [this, app_hdr](int index) {
        struct TabMeta {
            const char* icon;
            const char* title;
            const char* subtitle;
        };
        static constexpr std::array<TabMeta, 3> kTabs = {{
            {.icon = ":/icons/icons/panel_app_install.svg",
             .title = "App Installation",
             .subtitle = "Search, queue, and batch-install applications via Chocolatey"},
            {.icon = ":/icons/icons/panel_uninstall.svg",
             .title = "Advanced Uninstall",
             .subtitle = "Deep application removal with registry cleanup, leftover scanning, "
                         "and batch uninstall support"},
            {.icon = ":/icons/icons/icons8-warning-shield.svg",
             .title = "Vulnerability Scanner",
             .subtitle =
                 "Check installed software and packages against CISA KEV, NVD, GitHub Advisories, "
                 "and OSV"},
        }};
        if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
            const auto& m = kTabs[index];
            sak::updatePanelHeader(app_hdr,
                                   QString::fromUtf8(m.icon),
                                   QCoreApplication::translate("MainWindow", m.title),
                                   QCoreApplication::translate("MainWindow", m.subtitle));
        }
        updateVulnerabilityStatusBarVisibility();
    });

    addTabWithTooltip(m_tab_widget,
                      app_mgmt_wrapper,
                      "Application Management",
                      kTooltipAppManagement,
                      ":/icons/icons/panel_app_install.svg");
    updateVulnerabilityStatusBarVisibility();
    logInfo("MainWindow: Application Management panels initialized");
}

void MainWindow::createAppManagementChildPanels() {
    logInfo("MainWindow: creating Application Management panels");
    logInfo("MainWindow: creating App Installation panel");
    m_app_installation_panel = std::make_unique<AppInstallationPanel>(this);
    logInfo("MainWindow: App Installation panel initialized");
    logInfo("MainWindow: creating Advanced Uninstall panel");
    m_advanced_uninstall_panel = std::make_unique<AdvancedUninstallPanel>(this);
    logInfo("MainWindow: Advanced Uninstall panel initialized");
    logInfo("MainWindow: creating Vulnerability Scanner panel");
    m_vulnerability_panel = std::make_unique<VulnerabilityPanel>(this);
    logInfo("MainWindow: Vulnerability Scanner panel initialized");
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

    auto* net_mgmt_wrapper = new QWidget(this);
    auto* net_mgmt_layout = new QVBoxLayout(net_mgmt_wrapper);
    net_mgmt_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    net_mgmt_layout->setSpacing(ui::kSpacingDefault);

    auto net_hdr = sak::createDynamicPanelHeader(
        net_mgmt_wrapper,
        QStringLiteral(":/icons/icons/panel_network.svg"),
        tr("Network Diagnostics & Troubleshooting"),
        tr("Comprehensive network analysis -- connectivity testing, DNS diagnostics, "
           "port scanning, bandwidth, WiFi analysis, firewall auditing, and more"),
        net_mgmt_layout);

    auto* net_tabs = new QTabWidget(net_mgmt_wrapper);
    net_tabs->setAccessibleName(tr("Network management tabs"));
    net_tabs->addTab(m_network_diagnostic_panel.get(), tr("Network Diagnostics"));
    net_tabs->addTab(m_network_diagnostic_panel->adapterWidget(), tr("Network Adapters"));
    net_tabs->addTab(m_wifi_manager_panel.get(), tr("WiFi Manager"));
    net_mgmt_layout->addWidget(net_tabs, 1);

    connectNetworkAdapterLogToggle();

    connect(net_tabs, &QTabWidget::currentChanged, this, [net_hdr](int index) {
        struct TabMeta {
            const char* icon;
            const char* title;
            const char* subtitle;
        };
        static constexpr std::array<TabMeta, 3> kTabs = {{
            {.icon = ":/icons/icons/panel_network.svg",
             .title = "Network Diagnostics & Troubleshooting",
             .subtitle = "Comprehensive network analysis "
                         "\xe2\x80\x94 connectivity testing, DNS diagnostics, "
                         "port scanning, bandwidth, WiFi analysis, "
                         "firewall auditing, and more"},
            {.icon = ":/icons/icons/icons8-network-card.svg",
             .title = "Network Adapters",
             .subtitle = "View and manage network adapter configurations, "
                         "backup and restore Ethernet settings"},
            {.icon = ":/icons/icons/panel_wifi.svg",
             .title = "WiFi Manager",
             .subtitle = "Manage, share, and deploy Wi-Fi network profiles"},
        }};
        if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
            const auto& m = kTabs[index];
            sak::updatePanelHeader(net_hdr,
                                   QString::fromUtf8(m.icon),
                                   QCoreApplication::translate("MainWindow", m.title),
                                   QCoreApplication::translate("MainWindow", m.subtitle));
        }
    });

    addTabWithTooltip(m_tab_widget,
                      net_mgmt_wrapper,
                      "Network Management",
                      kTooltipNetworkManagement,
                      ":/icons/icons/panel_network.svg");
}

void MainWindow::connectNetworkAdapterLogToggle() {
    auto* adapter_toggle = m_network_diagnostic_panel->adapterLogToggle();
    if ((adapter_toggle == nullptr) || (m_logWindow == nullptr)) {
        return;
    }
    connect(adapter_toggle,
            &LogToggleSwitch::toggled,
            m_logWindow,
            &DetachableLogWindow::setLogVisible);
    connect(m_logWindow,
            &DetachableLogWindow::visibilityChanged,
            adapter_toggle,
            &LogToggleSwitch::setChecked);
    // Same paired Dark toggle every other Log switch gets via connectPanelLog.
    attachThemeToggleToLogToggle(adapter_toggle);
}

void MainWindow::loadAboutPanelIcon(QLabel* icon_label) {
    Q_ASSERT(icon_label);
    Q_ASSERT(!QCoreApplication::applicationDirPath().isEmpty());
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList splash_candidates = {app_dir + "/sak_splash.png",
                                           app_dir + "/resources/sak_splash.png",
                                           app_dir + "/../resources/sak_splash.png",
                                           app_dir + "/../sak_splash.png"};
    QPixmap splash_pix;
    for (const auto& path : splash_candidates) {
        if (QFileInfo::exists(path)) {
            splash_pix.load(path);
            break;
        }
    }
    if (!splash_pix.isNull()) {
        icon_label->setPixmap(
            splash_pix.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        icon_label->setStyleSheet(ui::mainIconFallbackStyle());
    }
}

void MainWindow::createAboutPanel() {
    Q_ASSERT(m_tab_widget);
    auto* about_panel = new QWidget(this);
    auto* about_layout = new QVBoxLayout(about_panel);
    about_layout->setSpacing(ui::kSpacingLarge);
    about_layout->setContentsMargins(
        ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge);

    // Header  --  use splash screen image as icon
    auto* header_layout = new QHBoxLayout();
    auto* icon_label = new QLabel(about_panel);
    icon_label->setFixedSize(sak::kIconSize, sak::kIconSize);
    icon_label->setAccessibleName(QStringLiteral("S.A.K. Utility application icon"));
    loadAboutPanelIcon(icon_label);
    header_layout->addWidget(icon_label);

    auto* title_layout = new QVBoxLayout();
    auto* title = new QLabel(QStringLiteral("<b>S.A.K. Utility</b>"), about_panel);
    title->setStyleSheet(ui::fontSizeWeightColorStyle(
        ui::kFontSizeTitle, ui::kFontWeightBold, ui::kColorTextPrimary));
    title_layout->addWidget(title);

    auto* ver = new QLabel(QString("Version %1 \u2014 %2").arg(get_version(), get_build_date()),
                           about_panel);
    ver->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeBody));
    title_layout->addWidget(ver);

    header_layout->addLayout(title_layout);
    header_layout->addStretch();
    about_layout->addLayout(header_layout);

    // Tabs inside about panel  --  all use QTextBrowser for uniform look
    auto* about_tabs = new QTabWidget(about_panel);
    about_tabs->setAccessibleName(tr("About information tabs"));
    about_tabs->addTab(createHtmlBrowser(about_panel, kAboutTabHtml, tr("About S.A.K. Utility")),
                       QStringLiteral("About"));
    about_tabs->addTab(createHtmlBrowser(about_panel, kLicenseTabHtml, tr("License text")),
                       QStringLiteral("License"));
    about_tabs->addTab(createHtmlBrowser(about_panel, kCreditsTabHtml, tr("Third-party credits")),
                       QStringLiteral("Credits"));

    about_layout->addWidget(about_tabs);
    const int about_idx =
        m_tab_widget->addTab(about_panel,
                             QIcon(QStringLiteral(":/icons/icons/panel_about.svg")),
                             QStringLiteral("About"));
    m_tab_widget->setIconSize(QSize(kTabIconSize, kTabIconSize));
    Q_UNUSED(about_idx);
}

void MainWindow::createHelpPanel() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(!QCoreApplication::applicationName().isEmpty());

    auto* help_panel = new QWidget(this);
    auto* help_layout = new QVBoxLayout(help_panel);
    help_layout->setSpacing(ui::kSpacingLarge);
    help_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);

    // Panel header -- consistent with other panels
    auto* header_widget = new QWidget(help_panel);
    auto* header_layout = new QVBoxLayout(header_widget);
    header_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    sak::createPanelHeader(header_widget,
                           QStringLiteral(":/icons/icons/panel_help.svg"),
                           tr("Help and Support"),
                           tr("Get help, report issues, or request features for S.A.K. Utility."),
                           header_layout);
    help_layout->addWidget(header_widget);

    help_layout->addLayout(createHelpRow_requestsAndBugs(help_panel));

    help_layout->addLayout(createHelpRow_wikiAndCommunity(help_panel));

    help_layout->addStretch();

    const int help_idx = m_tab_widget->addTab(help_panel,
                                              QIcon(QStringLiteral(":/icons/icons/panel_help.svg")),
                                              QStringLiteral("Help and Support"));
    m_tab_widget->setIconSize(QSize(kTabIconSize, kTabIconSize));
    Q_UNUSED(help_idx);
}

// ============================================================================
// Help Panel -- Card Row Builders
// ============================================================================

QHBoxLayout* MainWindow::createHelpRow_requestsAndBugs(QWidget* parent) {
    Q_ASSERT(parent);

    auto* row = new QHBoxLayout();
    row->setSpacing(ui::kSpacingLarge);

    auto* feature_btn = new QPushButton(tr("Request a Feature"), parent);
    feature_btn->setMinimumHeight(sak::kButtonHeightTall);
    feature_btn->setStyleSheet(ui::kPrimaryButtonStyle);
    feature_btn->setAccessibleName(QStringLiteral("Submit feature request on GitHub"));
    feature_btn->setToolTip(
        QStringLiteral("Opens a GitHub issue form to submit a feature request"));
    connect(feature_btn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/RandyNorthrup/S.A.K.-Utility/issues/new"
                                "?template=feature_request.yml&title=%5BFeature+Request%5D%3A+")));
    });

    row->addWidget(createActionCard(parent,
                                    QStringLiteral(":/icons/icons/features.svg"),
                                    tr("Feature Requests"),
                                    tr("Have an idea to improve S.A.K. Utility?"
                                       " Submit a feature request on GitHub."),
                                    feature_btn)
                       .card);

    auto* bug_btn = new QPushButton(tr("Report a Bug"), parent);
    bug_btn->setMinimumHeight(sak::kButtonHeightTall);
    bug_btn->setStyleSheet(ui::kDangerButtonStyle);
    bug_btn->setAccessibleName(QStringLiteral("Report a bug on GitHub"));
    bug_btn->setToolTip(QStringLiteral("Opens a GitHub issue form to report a bug"));
    connect(bug_btn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/RandyNorthrup/S.A.K.-Utility/issues/new"
                                "?template=bug_report.yml&title=%5BBug%5D%3A+")));
    });

    row->addWidget(createActionCard(parent,
                                    QStringLiteral(":/icons/icons/bugs.svg"),
                                    tr("Report a Bug"),
                                    tr("Found something broken? Let us know so we can fix it."),
                                    bug_btn)
                       .card);

    return row;
}

QHBoxLayout* MainWindow::createHelpRow_wikiAndCommunity(QWidget* parent) {
    Q_ASSERT(parent);

    auto* row = new QHBoxLayout();
    row->setSpacing(ui::kSpacingLarge);

    auto* wiki_btn = new QPushButton(tr("Open Help Wiki"), parent);
    wiki_btn->setMinimumHeight(sak::kButtonHeightTall);
    wiki_btn->setStyleSheet(ui::kSecondaryButtonStyle);
    wiki_btn->setAccessibleName(QStringLiteral("Open help wiki on GitHub"));
    wiki_btn->setToolTip(QStringLiteral("Opens the S.A.K. Utility wiki on GitHub"));
    connect(wiki_btn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/RandyNorthrup/S.A.K.-Utility/wiki")));
    });

    row->addWidget(createActionCard(parent,
                                    QStringLiteral(":/icons/icons/help.svg"),
                                    tr("Help & Documentation"),
                                    tr("Browse the wiki for guides, FAQ, and troubleshooting."),
                                    wiki_btn)
                       .card);

    row->addWidget(createCommunityCard(parent));

    return row;
}

QFrame* MainWindow::createCommunityCard(QWidget* parent) {
    Q_ASSERT(parent);

    auto* discord_btn = new QPushButton(tr("Join Discord"), parent);
    discord_btn->setMinimumHeight(sak::kButtonHeightTall);
    discord_btn->setStyleSheet(ui::kDiscordCompactButtonStyle);
    discord_btn->setAccessibleName(QStringLiteral("Join S.A.K. Utility Discord server"));
    discord_btn->setToolTip(QStringLiteral("Opens the general discussion Discord channel"));
    connect(discord_btn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://discord.gg/pMh2n9kSK3")));
    });

    return createActionCard(parent,
                            QStringLiteral(":/icons/icons/discord.svg"),
                            tr("Community"),
                            tr("Join the Discord server for general discussion, help, and "
                               "announcements."),
                            discord_btn)
        .card;
}

void MainWindow::wireBackupPanel() {
    Q_ASSERT(m_user_migration_panel);
    connect(m_user_migration_panel.get(),
            &UserMigrationPanel::statusMessage,
            this,
            [this](const QString& msg) { updateStatus(msg, kDefaultPanelStatusTimeoutMs); });
    connectPanelLog(m_user_migration_panel.get());
}

void MainWindow::wireFileManagementPanels() {
    Q_ASSERT(m_organizer_panel);
    connect(m_organizer_panel.get(),
            &OrganizerPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_organizer_panel.get(),
            &OrganizerPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connect(m_advanced_search_panel.get(),
            &AdvancedSearchPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_advanced_search_panel.get(),
            &AdvancedSearchPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connectPanelLog(m_organizer_panel.get());
    connectPanelLog(m_advanced_search_panel.get());
}

void MainWindow::wirePartitionPanel() {
    Q_ASSERT(m_partition_manager_panel);
    connect(m_partition_manager_panel.get(),
            &PartitionManagerPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_partition_manager_panel.get(),
            &PartitionManagerPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connectPartitionManagerNavigation();
    connectPanelLog(m_partition_manager_panel.get());
}

void MainWindow::connectPartitionManagerNavigation() {
    // Navigate by fixed lazy-tab slot: setCurrentIndex materializes the target.
    connect(m_partition_manager_panel.get(),
            &PartitionManagerPanel::openBenchmarkRequested,
            this,
            [this]() {
                if (m_slotDiagnostic >= 0) {
                    m_tab_widget->setCurrentIndex(m_slotDiagnostic);
                    updateStatus(tr("Opened Benchmark and Diagnostics for disk benchmark"),
                                 kDefaultPanelStatusTimeoutMs);
                }
            });
    connect(m_partition_manager_panel.get(),
            &PartitionManagerPanel::openImageFlasherRequested,
            this,
            [this]() {
                if (m_slotImageFlasher >= 0) {
                    m_tab_widget->setCurrentIndex(m_slotImageFlasher);
                    updateStatus(tr("Opened Image Flasher for bootable media"),
                                 kDefaultPanelStatusTimeoutMs);
                }
            });
}

void MainWindow::wireImageFlasherPanel() {
    Q_ASSERT(m_image_flasher_panel);
    connect(m_image_flasher_panel.get(),
            &ImageFlasherPanel::statusMessage,
            this,
            [this](const QString& msg) { updateStatus(msg, kDefaultPanelStatusTimeoutMs); });
    connect(m_image_flasher_panel.get(),
            &ImageFlasherPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connectPanelLog(m_image_flasher_panel.get());
}

void MainWindow::wireDiagnosticPanel() {
    Q_ASSERT(m_diagnostic_benchmark_panel);
    connect(m_diagnostic_benchmark_panel.get(),
            &DiagnosticBenchmarkPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_diagnostic_benchmark_panel.get(),
            &DiagnosticBenchmarkPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connectPanelLog(m_diagnostic_benchmark_panel.get());
}

void MainWindow::wireEmailPanels() {
    Q_ASSERT(m_email_inspector_panel);
    connect(m_email_inspector_panel.get(),
            &EmailInspectorPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_email_inspector_panel.get(),
            &EmailInspectorPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connectPanelLog(m_email_inspector_panel.get());
}

void MainWindow::wireAppManagementPanels() {
    Q_ASSERT(m_app_installation_panel);
    connect(m_app_installation_panel.get(),
            &AppInstallationPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_app_installation_panel.get(),
            &AppInstallationPanel::progressUpdated,
            this,
            &MainWindow::updateProgress);
    connect(m_advanced_uninstall_panel.get(),
            &AdvancedUninstallPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_advanced_uninstall_panel.get(),
            &AdvancedUninstallPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connect(m_vulnerability_panel.get(),
            &VulnerabilityPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_vulnerability_panel.get(),
            &VulnerabilityPanel::summaryStatusChanged,
            this,
            [this](const QString& summary) {
                if (m_vulnerability_summary_label) {
                    m_vulnerability_summary_label->setText(summary);
                    m_vulnerability_summary_label->setToolTip(summary);
                }
                updateVulnerabilityStatusBarVisibility();
            });
    connectPanelLog(m_app_installation_panel.get());
    connectPanelLog(m_vulnerability_panel.get());
    connectPanelLog(m_advanced_uninstall_panel.get());
    updateVulnerabilityStatusBarVisibility();
}

void MainWindow::wireNetworkPanels() {
    Q_ASSERT(m_network_diagnostic_panel);
    connect(m_network_diagnostic_panel.get(),
            &NetworkDiagnosticPanel::statusMessage,
            this,
            [this](const QString& msg, int timeout_ms) {
                updateStatus(msg, timeout_ms > 0 ? timeout_ms : kDefaultPanelStatusTimeoutMs);
            });
    connect(m_network_diagnostic_panel.get(),
            &NetworkDiagnosticPanel::progressUpdate,
            this,
            &MainWindow::updateProgress);
    connectPanelLog(m_network_diagnostic_panel.get());
    if (m_wifi_manager_panel) {
        connectPanelLog(m_wifi_manager_panel.get());
    }
}

#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
void MainWindow::wireAiPanel() {
    if (!m_ai_assistant_panel) {
        return;
    }
    connect(m_ai_assistant_panel.get(),
            &AiAssistantPanel::statusDetailsChanged,
            this,
            [this](const QString& details) {
                if (m_ai_status_label) {
                    m_ai_status_label->setText(details);
                    // The details embed run-supplied ids (workflow phase, tool command); a
                    // tooltip has no plain-text mode, so wrap it to be shown literally.
                    m_ai_status_label->setToolTip(sak::ui::asLiteralRichText(details));
                }
                updateAiStatusBarVisibility();
            });
    connectPanelLog(m_ai_assistant_panel.get());
    updateAiStatusBarVisibility();
}
#endif

bool MainWindow::isVulnerabilityPanelActive() const {
    return (m_tab_widget != nullptr) && (m_application_tabs != nullptr) && m_vulnerability_panel &&
           m_tab_widget->currentIndex() == findPanelTabIndex(m_vulnerability_panel.get()) &&
           m_application_tabs->currentWidget() == m_vulnerability_panel.get();
}

void MainWindow::updateVulnerabilityStatusBarVisibility() {
    if (m_shutting_down || (m_vulnerability_summary_label == nullptr)) {
        return;
    }

    const bool active = isVulnerabilityPanelActive();
    if (active && m_vulnerability_panel) {
        const QString summary = m_vulnerability_panel->statusSummary();
        m_vulnerability_summary_label->setText(summary);
        m_vulnerability_summary_label->setToolTip(summary);
    }
    m_vulnerability_summary_label->setVisible(active &&
                                              !m_vulnerability_summary_label->text().isEmpty());
}

#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
bool MainWindow::isAiAssistantPanelActive() const {
    return (m_tab_widget != nullptr) && m_ai_assistant_panel &&
           m_tab_widget->currentIndex() == findPanelTabIndex(m_ai_assistant_panel.get());
}

void MainWindow::updateAiStatusBarVisibility() {
    if (m_shutting_down || (m_ai_status_label == nullptr)) {
        return;
    }

    const bool active = isAiAssistantPanelActive();
    if (active && m_ai_assistant_panel) {
        const QString details = m_ai_assistant_panel->statusDetails();
        m_ai_status_label->setText(details);
        m_ai_status_label->setToolTip(sak::ui::asLiteralRichText(details));
    }
    m_ai_status_label->setVisible(active && !m_ai_status_label->text().isEmpty());
}
#endif

int MainWindow::findPanelTabIndex(QWidget* panel) const {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(panel);

    int idx = m_tab_widget->indexOf(panel);
    if (idx >= 0) {
        return idx;
    }
    // Walk up through parent widgets to find the wrapper
    // that is a direct child of the main tab widget.
    const QWidget* widget = panel->parentWidget();
    while ((widget != nullptr) && widget != m_tab_widget) {
        idx = m_tab_widget->indexOf(widget);
        if (idx >= 0) {
            return idx;
        }
        widget = widget->parentWidget();
    }
    return -1;
}

template <typename PanelT>
void MainWindow::connectPanelLog(PanelT* panel) {
    Q_ASSERT(panel);
    Q_ASSERT(m_logWindow);
    const int tab_idx = findPanelTabIndex(panel);
    connect(panel, &PanelT::logOutput, this, [this, tab_idx](const QString& msg) {
        const QString formatted = QDateTime::currentDateTime().toString("[HH:mm:ss] ") + msg;
        appendLogIfActive(tab_idx, formatted);
    });

    auto* toggle = panel->logToggle();
    if (toggle) {
        connect(
            toggle, &LogToggleSwitch::toggled, m_logWindow, &DetachableLogWindow::setLogVisible);
        connect(m_logWindow,
                &DetachableLogWindow::visibilityChanged,
                toggle,
                &LogToggleSwitch::setChecked);
        attachThemeToggleToLogToggle(toggle);
    }
}

void MainWindow::setupLogRouting() {
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(m_logWindow);

    // Switch log content when tabs change
    connect(m_tab_widget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Re-populate log when the log window becomes visible
    connect(m_logWindow, &DetachableLogWindow::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            onTabChanged(m_tab_widget->currentIndex());
        }
    });
}

void MainWindow::attachThemeToggleToLogToggle(LogToggleSwitch* log_toggle) {
    if ((log_toggle == nullptr) || (log_toggle->parentWidget() == nullptr)) {
        return;
    }

    int item_index = -1;
    QBoxLayout* box_layout =
        findBoxLayoutContainingWidget(log_toggle->parentWidget()->layout(), log_toggle, item_index);
    if ((box_layout == nullptr) || item_index < 0) {
        return;
    }

    auto* theme_toggle = new LogToggleSwitch(tr("Dark"), log_toggle->parentWidget());
    theme_toggle->setToolTip(tr("Toggle dark mode"));
    theme_toggle->setAccessibleName(tr("Toggle dark mode"));
    theme_toggle->setChecked(m_dark_theme_enabled);
    m_theme_toggles.append(theme_toggle);

    connect(theme_toggle, &LogToggleSwitch::toggled, this, &MainWindow::setDarkThemeEnabled);
    box_layout->insertWidget(item_index + 1, theme_toggle);
}

void MainWindow::setDarkThemeEnabled(bool enabled) {
    if (m_dark_theme_enabled == enabled) {
        return;
    }

    m_dark_theme_enabled = enabled;
    if (qApp) {
        sak::ui::applyWindows11Theme(
            *qApp, enabled ? sak::ui::AppThemeMode::Dark : sak::ui::AppThemeMode::Light);
    }

    for (auto* toggle : m_theme_toggles) {
        if (toggle == nullptr) {
            continue;
        }
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(enabled);
    }
    applyThemedHtmlBrowsers(this);
}

void MainWindow::updateStatus(const QString& message, int timeout_ms) {
    if (m_status_label != nullptr) {
        if (timeout_ms > 0) {
            statusBar()->showMessage(message, timeout_ms);
        } else {
            m_status_label->setText(message);
        }
    }
}

void MainWindow::updateProgress(int current, int maximum) {
    // Public slot, connected to ten panel signals whose counts come from parsed
    // files (a PST content count is a signed int32 read straight off disk). A
    // negative bound is not a display problem to smooth over -- it means the
    // producer's count is wrong, so say so and leave the bar showing the last
    // value it was actually given rather than a fabricated one.
    if (current < 0 || maximum < 0) {
        logError("MainWindow::updateProgress refused a negative range: current={} maximum={}",
                 current,
                 maximum);
        return;
    }
    if (m_progress_bar == nullptr) {
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
        QTimer::singleShot(sak::kTimerProgressCompleteHoldMs,
                           this,
                           &MainWindow::hideProgressBarIfComplete);
    } else if (maximum > 0) {
        m_progress_bar->setVisible(true);
    }
}

void MainWindow::hideProgressBarIfComplete() {
    if ((m_progress_bar != nullptr) && m_progress_bar->value() >= m_progress_bar->maximum()) {
        m_progress_bar->setVisible(false);
    }
}

void MainWindow::appendLogIfActive(int tab_idx, const QString& formatted) {
    // m_panelLogs is declared last and destroyed first; a panel destructor that
    // emits logOutput during ~MainWindow would write into a destroyed QMap.
    if (m_shutting_down) {
        return;
    }
    Q_ASSERT(m_tab_widget);
    QStringList& lines = m_panelLogs[tab_idx];
    lines.append(formatted);
    // Bound the per-tab backing store: evict the oldest lines once the cap is exceeded so
    // heavy untrusted output cannot grow this map without limit for the life of the session.
    if (lines.size() > kMaxPanelLogLines) {
        const qsizetype overflow = lines.size() - kMaxPanelLogLines;
        lines.erase(lines.cbegin(), lines.cbegin() + overflow);
    }
    if (m_tab_widget->currentIndex() == tab_idx && m_logWindow->isLogVisible()) {
        // Panel log payloads carry paths, program names and command output; QTextEdit::append()
        // would render markup inside them, so wrap the line to read literally.
        m_logWindow->logTextEdit()->append(sak::ui::asLiteralRichText(formatted));
    }
}

void MainWindow::setProgressVisible(bool visible) {
    if (m_progress_bar != nullptr) {
        m_progress_bar->setVisible(visible);
    }
}


void MainWindow::onTabChanged(int index) {
    if (m_shutting_down) {
        return;
    }
    Q_ASSERT(m_tab_widget);
    Q_ASSERT(index >= -1 && index < m_tab_widget->count());
    updateVulnerabilityStatusBarVisibility();
#if defined(SAK_ENABLE_AI_ASSISTANT) && SAK_ENABLE_AI_ASSISTANT
    updateAiStatusBarVisibility();
#endif
    if (m_logWindow == nullptr) {
        return;
    }

    // Replace log content with the active panel's accumulated log
    m_logWindow->logTextEdit()->clear();
    if (m_panelLogs.contains(index)) {
        for (const auto& line : m_panelLogs[index]) {
            m_logWindow->logTextEdit()->append(sak::ui::asLiteralRichText(line));
        }
    }
}

void MainWindow::createKeyboardShortcuts() {
    Q_ASSERT(m_logWindow);
    Q_ASSERT(m_tab_widget);
    const int tab_count = m_tab_widget->count();

    // Tab navigation: Ctrl+1..9 switches to tab index 0..8
    for (int i = 0; i < qMin(tab_count, kDirectTabShortcutCount); ++i) {
        auto* shortcut = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)),
                                       this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, i]() {
            m_tab_widget->setCurrentIndex(i);
        });
    }

    // Ctrl+0: switches to tab index 9 (10th tab) -- browser convention
    if (tab_count > kTenthTabIndex) {
        auto* tab10 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
        tab10->setContext(Qt::ApplicationShortcut);
        connect(tab10, &QShortcut::activated, this, [this]() {
            m_tab_widget->setCurrentIndex(kTenthTabIndex);
        });
    }

    // Ctrl+Shift+1..5: switches to tab index 10..14 (tabs beyond 10)
    for (int i = 0; i < qMin(tab_count - kTabShortcutBaseOffset, kShiftTabShortcutCount); ++i) {
        const int tab_idx = kTabShortcutBaseOffset + i;
        if (tab_idx >= tab_count) {
            break;
        }
        auto* shortcut = new QShortcut(
            QKeySequence(Qt::CTRL | Qt::SHIFT | static_cast<Qt::Key>(Qt::Key_1 + i)), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, tab_idx]() {
            m_tab_widget->setCurrentIndex(tab_idx);
        });
    }

    // Ctrl+Tab / Ctrl+Shift+Tab: cycle through tabs
    auto* next_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), this);
    next_tab->setContext(Qt::ApplicationShortcut);
    connect(next_tab, &QShortcut::activated, this, [this]() {
        const int next = (m_tab_widget->currentIndex() + 1) % m_tab_widget->count();
        m_tab_widget->setCurrentIndex(next);
    });

    auto* prev_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this);
    prev_tab->setContext(Qt::ApplicationShortcut);
    connect(prev_tab, &QShortcut::activated, this, [this]() {
        const int prev = (m_tab_widget->currentIndex() - 1 + m_tab_widget->count()) %
                         m_tab_widget->count();
        m_tab_widget->setCurrentIndex(prev);
    });

    // Ctrl+Shift+L: Toggle log panel visibility. Plain Ctrl+L belongs to the
    // File Explorer omnibar (Files EditPathAction); an application-wide
    // Ctrl+L here made the two ambiguous whenever the explorer had focus and
    // Qt then fired neither shortcut.
    auto* toggle_log = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")), this);
    toggle_log->setContext(Qt::ApplicationShortcut);
    connect(toggle_log, &QShortcut::activated, this, [this]() {
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

    // Always start on the first registered panel. With AI enabled this is AI Assistant.
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
    if (m_logWindow != nullptr) {
        m_logWindow->repositionIfAnchored();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    Q_ASSERT(event);
    QMainWindow::resizeEvent(event);
    if (m_logWindow != nullptr) {
        m_logWindow->repositionIfAnchored();
    }
    // Scroll buttons may appear/disappear on resize
    QTimer::singleShot(0, this, &MainWindow::applyTabBarChevrons);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (m_lazyDefaultBuilt) {
        return;
    }
    m_lazyDefaultBuilt = true;
    // Let the empty shell paint first, then build the initially-selected panel
    // off the critical path so the window becomes visible immediately.
    QTimer::singleShot(0, this, [this]() { materializeTab(m_tab_widget->currentIndex()); });
}

void MainWindow::applyTabBarChevrons() {
    if (m_tab_widget == nullptr) {
        return;
    }

    const auto buttons = m_tab_widget->tabBar()->findChildren<QToolButton*>();
    for (auto* btn : buttons) {
        const Qt::ArrowType arrow_type = btn->arrowType();
        QString direction = btn->property(kTabScrollDirectionProperty).toString();
        if (arrow_type == Qt::LeftArrow) {
            direction = QString::fromLatin1(kTabScrollDirectionLeft);
        } else if (arrow_type == Qt::RightArrow) {
            direction = QString::fromLatin1(kTabScrollDirectionRight);
        } else if (direction.isEmpty()) {
            direction = btn == buttons.value(0) ? QString::fromLatin1(kTabScrollDirectionLeft)
                                                : QString::fromLatin1(kTabScrollDirectionRight);
        }
        btn->setProperty(kTabScrollDirectionProperty, direction);
        btn->setObjectName(QString::fromLatin1(kTabScrollButtonObjectName));
        btn->setArrowType(Qt::NoArrow);
        btn->setText(QString());
        btn->setAutoRaise(false);
        btn->setFixedSize(ui::kUiTabScrollButtonWidth, ui::kUiTabScrollButtonHeight);
        btn->setIconSize(QSize(ui::kUiTabScrollIconSize, ui::kUiTabScrollIconSize));
        btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        btn->setStyleSheet(ui::tabScrollButtonStyle());
        if (direction == QLatin1String(kTabScrollDirectionLeft)) {
            btn->setIcon(ui::selectorChevronLeftToolButtonIcon());
            btn->setToolTip(tr("Scroll tabs left"));
            btn->setAccessibleName(tr("Scroll tabs left"));
        } else {
            btn->setIcon(ui::selectorChevronRightToolButtonIcon());
            btn->setToolTip(tr("Scroll tabs right"));
            btn->setAccessibleName(tr("Scroll tabs right"));
        }
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
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
