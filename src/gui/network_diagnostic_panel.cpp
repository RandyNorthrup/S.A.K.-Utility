// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_panel.cpp
/// @brief Main UI panel for Network Diagnostics & Troubleshooting

#include "sak/network_diagnostic_panel.h"

#include "sak/actions/reset_network_action.h"
#include "sak/detachable_log_window.h"
#include "sak/dns_diagnostic_tool.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/network_diagnostic_controller.h"
#include "sak/port_scanner.h"
#include "sak/quick_action_controller.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QShortcut>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace sak {

// ===================================================================
// Construction / Destruction
// ===================================================================

NetworkDiagnosticPanel::NetworkDiagnosticPanel(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<NetworkDiagnosticController>(this)) {
    setupUi();
    connectSignals();
    createResetNetworkAction();

    // Initial adapter scan
    QMetaObject::invokeMethod(m_controller.get(),
                              &NetworkDiagnosticController::scanAdapters,
                              Qt::QueuedConnection);
}

NetworkDiagnosticPanel::~NetworkDiagnosticPanel() = default;

// ===================================================================
// UI Setup
// ===================================================================

void NetworkDiagnosticPanel::setupUi() {
    // Root layout -- compact header, splitter for adapter+tools, report pinned at bottom.
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    rootLayout->setSpacing(ui::kSpacingSmall);

    // Adapter section -- exposed via adapterWidget() for outer tab placement
    m_adapterWidget = createAdapterSection();

    // Diagnostic tool tabs
    m_toolTabs = new QTabWidget(this);
    m_toolTabs->addTab(createPingTab(), tr("Ping"));
    m_toolTabs->addTab(createTracerouteTab(), tr("Traceroute"));
    m_toolTabs->addTab(createMtrTab(), tr("MTR"));
    m_toolTabs->addTab(createDnsTab(), tr("DNS"));
    m_toolTabs->addTab(createPortScanTab(), tr("Port Scan"));
    m_toolTabs->addTab(createBandwidthTab(), tr("Bandwidth"));
    m_toolTabs->addTab(createWiFiTab(), tr("WiFi"));
    m_toolTabs->addTab(createConnectionsTab(), tr("Connections"));
    m_toolTabs->addTab(createFirewallTab(), tr("Firewall"));
    m_toolTabs->addTab(createSharesTab(), tr("Shares"));
    m_toolTabs->addTab(createLanTransferTab(), tr("LAN Transfer"));
    setAccessible(m_toolTabs,
                  tr("Diagnostic tools"),
                  tr("Tab widget for selecting network diagnostic tools"));
    rootLayout->addWidget(m_toolTabs, 1);

    // Status bar with log toggle
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 2, 0, 0);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    statusRow->addWidget(m_logToggle);
    statusRow->addStretch();
    rootLayout->addLayout(statusRow);

    setupKeyboardShortcuts();
}

void NetworkDiagnosticPanel::setupKeyboardShortcuts() {
    Q_ASSERT(m_controller);
    auto* refreshShortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
    refreshShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(
        refreshShortcut, &QShortcut::activated, this, &NetworkDiagnosticPanel::onRefreshAdapters);

    auto* cancelShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    cancelShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cancelShortcut, &QShortcut::activated, this, [this]() {
        if (m_controller->currentState() != NetworkDiagnosticController::State::Idle) {
            m_controller->cancel();
        }
    });
}

// -- Adapter Section -----------------------------------------------------

QWidget* NetworkDiagnosticPanel::createAdapterSection() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupAdapterToolbar(widget, layout);
    setupAdapterTable(widget, layout);
    setupAdapterDetailLabel(widget, layout);

    // Log toggle for adapter tab
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 2, 0, 0);
    m_adapterLogToggle = new LogToggleSwitch(tr("Log"), widget);
    statusRow->addWidget(m_adapterLogToggle);
    statusRow->addStretch();
    layout->addLayout(statusRow);

    return widget;
}

void NetworkDiagnosticPanel::setupAdapterToolbar(QWidget* parent, QVBoxLayout* layout) {
    auto* toolbar = new QHBoxLayout();
    m_refreshBtn = new QPushButton(tr("Refresh Adapters"), parent);
    m_refreshBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_refreshBtn->setToolTip(tr("Re-scan all network adapters"));
    setAccessible(m_refreshBtn,
                  tr("Refresh adapters"),
                  tr("Re-enumerate all network adapters and their configurations"));
    toolbar->addWidget(m_refreshBtn);

    m_copyConfigBtn = new QPushButton(tr("Copy Config"), parent);
    m_copyConfigBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_copyConfigBtn->setToolTip(tr("Copy selected adapter configuration to clipboard"));
    m_copyConfigBtn->setEnabled(false);
    setAccessible(m_copyConfigBtn, tr("Copy adapter config"));
    toolbar->addWidget(m_copyConfigBtn);

    m_backupEthernetBtn = new QPushButton(tr("Backup Settings"), parent);
    m_backupEthernetBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_backupEthernetBtn->setToolTip(
        tr("Backup selected Ethernet adapter IP/DNS settings to a JSON file "
           "for restoration on this or another PC"));
    setAccessible(m_backupEthernetBtn,
                  tr("Backup Ethernet settings"),
                  tr("Save selected adapter's IP configuration to a portable JSON file"));
    m_backupEthernetBtn->setEnabled(false);
    toolbar->addWidget(m_backupEthernetBtn);

    m_restoreEthernetBtn = new QPushButton(tr("Restore Settings"), parent);
    m_restoreEthernetBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_restoreEthernetBtn->setToolTip(
        tr("Restore Ethernet adapter settings from a previously saved "
           "JSON backup file -- works across PCs"));
    setAccessible(m_restoreEthernetBtn,
                  tr("Restore Ethernet settings"),
                  tr("Load and apply adapter IP configuration from a backup JSON file"));
    toolbar->addWidget(m_restoreEthernetBtn);

    m_resetNetworkBtn = new QPushButton(tr("Reset Network"), parent);
    m_resetNetworkBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_resetNetworkBtn->setToolTip(
        tr("Reset all network settings including Winsock, TCP/IP, "
           "DNS cache, and firewall (requires admin)"));
    setAccessible(m_resetNetworkBtn,
                  tr("Reset network settings"),
                  tr("Perform a complete network stack reset"));
    toolbar->addWidget(m_resetNetworkBtn);

    toolbar->addStretch();
    layout->addLayout(toolbar);
}

void NetworkDiagnosticPanel::setupAdapterTable(QWidget* parent, QVBoxLayout* layout) {
    Q_ASSERT(layout);
    Q_ASSERT(parent);
    m_adapterTable = new QTableWidget(parent);
    m_adapterTable->setColumnCount(6);
    m_adapterTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Type"), tr("Status"), tr("IP Address"), tr("MAC"), tr("Speed")});
    m_adapterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_adapterTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_adapterTable->setAlternatingRowColors(true);
    m_adapterTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_adapterTable->verticalHeader()->setVisible(false);
    m_adapterTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* header = m_adapterTable->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::Interactive);
    header->resizeSection(1, 90);
    header->setSectionResizeMode(2, QHeaderView::Interactive);
    header->resizeSection(2, 90);
    header->setSectionResizeMode(3, QHeaderView::Interactive);
    header->resizeSection(3, 130);
    header->setSectionResizeMode(4, QHeaderView::Interactive);
    header->resizeSection(4, 140);
    header->setSectionResizeMode(5, QHeaderView::Interactive);
    header->resizeSection(5, 120);

    setAccessible(m_adapterTable,
                  tr("Network adapters"),
                  tr("List of network adapters with configuration details"));
    m_adapterTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_adapterTable, 1);
}

void NetworkDiagnosticPanel::setupAdapterDetailLabel(QWidget* parent, QVBoxLayout* layout) {
    Q_ASSERT(layout);
    Q_ASSERT(parent);
    const QString labelStyle = QStringLiteral("color: %1; font-size: %2pt;")
                                   .arg(ui::kColorTextMuted)
                                   .arg(ui::kFontSizeSmall);

    auto makeColumn = [&](QLabel*& label) -> QLabel* {
        label = new QLabel(parent);
        label->setWordWrap(true);
        label->setTextFormat(Qt::RichText);
        label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        label->setStyleSheet(labelStyle);
        return label;
    };

    auto* detailRow = new QHBoxLayout();
    detailRow->setSpacing(ui::kSpacingDefault);
    detailRow->setContentsMargins(4, 2, 4, 2);
    detailRow->addWidget(makeColumn(m_detailIdentity), 2);
    detailRow->addWidget(makeColumn(m_detailAddressing), 2);
    detailRow->addWidget(makeColumn(m_detailGatewayDns), 2);
    detailRow->addWidget(makeColumn(m_detailStatus), 1);

    auto* detailWidget = new QWidget(parent);
    detailWidget->setLayout(detailRow);
    detailWidget->setMinimumHeight(50);
    detailWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    layout->addWidget(detailWidget, 0);

    // Show placeholder
    m_detailIdentity->setText(tr("Select an adapter to view details"));
}

// -- Ping Tab ------------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createPingTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupPingConfig(widget, layout);
    setupPingControls(widget, layout);
    setupPingResults(widget, layout);

    return widget;
}

void NetworkDiagnosticPanel::setupPingConfig(QWidget* widget, QVBoxLayout* layout) {
    // Row 1: Target
    auto* targetRow = new QHBoxLayout();
    targetRow->addWidget(new QLabel(tr("Target:"), widget));
    m_pingTarget = new QLineEdit(widget);
    m_pingTarget->setPlaceholderText(tr("hostname or IP address"));
    m_pingTarget->setToolTip(tr("Target hostname or IP to ping"));
    setAccessible(m_pingTarget, tr("Ping target"));
    targetRow->addWidget(m_pingTarget, 2);
    layout->addLayout(targetRow);

    // Row 2: Count, Timeout, Interval, Size
    auto* optionsRow = new QHBoxLayout();
    optionsRow->addWidget(new QLabel(tr("Count:"), widget));
    m_pingCount = new QSpinBox(widget);
    m_pingCount->setRange(1, 1000);
    m_pingCount->setValue(10);
    m_pingCount->setToolTip(tr("Number of ping packets to send"));
    setAccessible(m_pingCount, tr("Ping count"), tr("Number of ICMP echo request packets to send"));
    optionsRow->addWidget(m_pingCount);

    optionsRow->addWidget(new QLabel(tr("Timeout:"), widget));
    m_pingTimeout = new QSpinBox(widget);
    m_pingTimeout->setRange(100, 30'000);
    m_pingTimeout->setValue(4000);
    m_pingTimeout->setSuffix(tr(" ms"));
    m_pingTimeout->setToolTip(tr("Timeout per ping in milliseconds"));
    setAccessible(m_pingTimeout, tr("Ping timeout"), tr("Maximum wait time for each ping reply"));
    optionsRow->addWidget(m_pingTimeout);

    optionsRow->addWidget(new QLabel(tr("Interval:"), widget));
    m_pingInterval = new QSpinBox(widget);
    m_pingInterval->setRange(100, 10'000);
    m_pingInterval->setValue(1000);
    m_pingInterval->setSuffix(tr(" ms"));
    m_pingInterval->setToolTip(tr("Delay between consecutive pings in milliseconds"));
    setAccessible(m_pingInterval,
                  tr("Ping interval"),
                  tr("Time between sending each ICMP echo request"));
    optionsRow->addWidget(m_pingInterval);

    optionsRow->addWidget(new QLabel(tr("Size:"), widget));
    m_pingPacketSize = new QSpinBox(widget);
    m_pingPacketSize->setRange(8, 65'500);
    m_pingPacketSize->setValue(32);
    m_pingPacketSize->setSuffix(tr(" B"));
    m_pingPacketSize->setToolTip(tr("ICMP packet payload size in bytes"));
    setAccessible(m_pingPacketSize, tr("Packet size"), tr("Size of the ICMP echo request payload"));
    optionsRow->addWidget(m_pingPacketSize);

    optionsRow->addStretch();
    layout->addLayout(optionsRow);
}

void NetworkDiagnosticPanel::setupPingControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btnRow = new QHBoxLayout();
    m_pingStartBtn = new QPushButton(tr("Start Ping"), widget);
    m_pingStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_pingStartBtn->setToolTip(tr("Send ICMP echo requests to the target host"));
    setAccessible(m_pingStartBtn, tr("Start ping test"));
    btnRow->addWidget(m_pingStartBtn);

    m_pingStopBtn = new QPushButton(tr("Stop"), widget);
    m_pingStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_pingStopBtn->setEnabled(false);
    m_pingStopBtn->setToolTip(tr("Cancel the current ping operation"));
    setAccessible(m_pingStopBtn, tr("Stop ping test"));
    btnRow->addWidget(m_pingStopBtn);

    btnRow->addStretch();
    m_pingStatsLabel = new QLabel(widget);
    m_pingStatsLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    btnRow->addWidget(m_pingStatsLabel);
    layout->addLayout(btnRow);
}

void NetworkDiagnosticPanel::setupPingResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(layout);
    Q_ASSERT(widget);
    m_pingTable = new QTableWidget(widget);
    m_pingTable->setColumnCount(5);
    m_pingTable->setHorizontalHeaderLabels(
        {tr("#"), tr("IP"), tr("Status"), tr("RTT (ms)"), tr("TTL")});
    m_pingTable->setAlternatingRowColors(true);
    m_pingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pingTable->verticalHeader()->setVisible(false);

    auto* pingHeader = m_pingTable->horizontalHeader();
    pingHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    pingHeader->resizeSection(0, 50);
    pingHeader->setSectionResizeMode(1, QHeaderView::Stretch);
    pingHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    pingHeader->resizeSection(2, 100);
    pingHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    pingHeader->resizeSection(3, 100);
    pingHeader->setSectionResizeMode(4, QHeaderView::Interactive);
    pingHeader->resizeSection(4, 60);

    setAccessible(m_pingTable, tr("Ping results"));
    m_pingTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_pingTable, 1);
}

// -- Traceroute Tab ------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createTracerouteTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupTracerouteConfig(widget, layout);
    setupTracerouteControls(widget, layout);
    setupTracerouteResults(widget, layout);

    return widget;
}

void NetworkDiagnosticPanel::setupTracerouteConfig(QWidget* widget, QVBoxLayout* layout) {
    auto* configRow = new QHBoxLayout();
    configRow->addWidget(new QLabel(tr("Target:"), widget));
    m_traceTarget = new QLineEdit(widget);
    m_traceTarget->setPlaceholderText(tr("hostname or IP address"));
    m_traceTarget->setToolTip(tr("Target hostname or IP address to trace"));
    setAccessible(m_traceTarget, tr("Traceroute target"));
    configRow->addWidget(m_traceTarget, 2);

    configRow->addWidget(new QLabel(tr("Max Hops:"), widget));
    m_traceMaxHops = new QSpinBox(widget);
    m_traceMaxHops->setRange(1, 64);
    m_traceMaxHops->setValue(30);
    m_traceMaxHops->setToolTip(tr("Maximum number of hops before giving up"));
    setAccessible(m_traceMaxHops, tr("Maximum hops"), tr("Maximum TTL value for the traceroute"));
    configRow->addWidget(m_traceMaxHops);
    layout->addLayout(configRow);
}

void NetworkDiagnosticPanel::setupTracerouteControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btnRow = new QHBoxLayout();
    m_traceStartBtn = new QPushButton(tr("Trace Route"), widget);
    m_traceStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_traceStartBtn->setToolTip(tr("Trace the network path to the target host"));
    setAccessible(m_traceStartBtn, tr("Start traceroute"));
    btnRow->addWidget(m_traceStartBtn);

    m_traceStopBtn = new QPushButton(tr("Stop"), widget);
    m_traceStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_traceStopBtn->setEnabled(false);
    m_traceStopBtn->setToolTip(tr("Cancel the current traceroute"));
    setAccessible(m_traceStopBtn, tr("Stop traceroute"));
    btnRow->addWidget(m_traceStopBtn);

    btnRow->addStretch();
    m_traceStatusLabel = new QLabel(widget);
    m_traceStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    btnRow->addWidget(m_traceStatusLabel);
    layout->addLayout(btnRow);
}

void NetworkDiagnosticPanel::setupTracerouteResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_traceTable = new QTableWidget(widget);
    m_traceTable->setColumnCount(7);
    m_traceTable->setHorizontalHeaderLabels({tr("Hop"),
                                             tr("IP"),
                                             tr("Hostname"),
                                             tr("RTT 1"),
                                             tr("RTT 2"),
                                             tr("RTT 3"),
                                             tr("Avg (ms)")});
    m_traceTable->setAlternatingRowColors(true);
    m_traceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_traceTable->verticalHeader()->setVisible(false);

    auto* trHeader = m_traceTable->horizontalHeader();
    trHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    trHeader->resizeSection(0, 50);
    trHeader->setSectionResizeMode(1, QHeaderView::Interactive);
    trHeader->resizeSection(1, 120);
    trHeader->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int i = 3; i < 7; ++i) {
        trHeader->setSectionResizeMode(i, QHeaderView::Interactive);
        trHeader->resizeSection(i, 80);
    }

    setAccessible(m_traceTable, tr("Traceroute results"));
    m_traceTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_traceTable, 1);
}

// -- MTR Tab -------------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createMtrTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupMtrConfig(widget, layout);
    setupMtrControls(widget, layout);
    setupMtrResults(widget, layout);

    return widget;
}

void NetworkDiagnosticPanel::setupMtrConfig(QWidget* widget, QVBoxLayout* layout) {
    auto* configRow = new QHBoxLayout();
    configRow->addWidget(new QLabel(tr("Target:"), widget));
    m_mtrTarget = new QLineEdit(widget);
    m_mtrTarget->setPlaceholderText(tr("hostname or IP address"));
    m_mtrTarget->setToolTip(tr("Target hostname or IP address for MTR analysis"));
    setAccessible(m_mtrTarget, tr("MTR target"));
    configRow->addWidget(m_mtrTarget, 2);

    configRow->addWidget(new QLabel(tr("Cycles:"), widget));
    m_mtrCycles = new QSpinBox(widget);
    m_mtrCycles->setRange(1, 1000);
    m_mtrCycles->setValue(100);
    m_mtrCycles->setToolTip(tr("Number of probe cycles to run"));
    setAccessible(m_mtrCycles,
                  tr("MTR cycles"),
                  tr("Number of complete pass cycles for the MTR analysis"));
    configRow->addWidget(m_mtrCycles);
    layout->addLayout(configRow);
}

void NetworkDiagnosticPanel::setupMtrControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btnRow = new QHBoxLayout();
    m_mtrStartBtn = new QPushButton(tr("Start MTR"), widget);
    m_mtrStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_mtrStartBtn->setToolTip(tr("Start combined traceroute and ping analysis"));
    setAccessible(m_mtrStartBtn, tr("Start MTR test"));
    btnRow->addWidget(m_mtrStartBtn);

    m_mtrStopBtn = new QPushButton(tr("Stop"), widget);
    m_mtrStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_mtrStopBtn->setEnabled(false);
    m_mtrStopBtn->setToolTip(tr("Cancel the current MTR analysis"));
    setAccessible(m_mtrStopBtn, tr("Stop MTR test"));
    btnRow->addWidget(m_mtrStopBtn);

    btnRow->addStretch();
    m_mtrStatusLabel = new QLabel(widget);
    m_mtrStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    btnRow->addWidget(m_mtrStatusLabel);
    layout->addLayout(btnRow);
}

void NetworkDiagnosticPanel::setupMtrResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_mtrTable = new QTableWidget(widget);
    m_mtrTable->setColumnCount(8);
    m_mtrTable->setHorizontalHeaderLabels({tr("Hop"),
                                           tr("IP/Hostname"),
                                           tr("Loss %"),
                                           tr("Sent"),
                                           tr("Avg (ms)"),
                                           tr("Best"),
                                           tr("Worst"),
                                           tr("Jitter")});
    m_mtrTable->setAlternatingRowColors(true);
    m_mtrTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mtrTable->verticalHeader()->setVisible(false);

    auto* mtrHeader = m_mtrTable->horizontalHeader();
    mtrHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    mtrHeader->resizeSection(0, 50);
    mtrHeader->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int i = 2; i < 8; ++i) {
        mtrHeader->setSectionResizeMode(i, QHeaderView::Interactive);
        mtrHeader->resizeSection(i, 75);
    }

    setAccessible(m_mtrTable, tr("MTR results"));
    m_mtrTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_mtrTable, 1);
}

// -- DNS Tab -------------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createDnsTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupDnsConfig(widget, layout);
    setupDnsControls(widget, layout);
    setupDnsResults(widget, layout);

    return widget;
}

void NetworkDiagnosticPanel::setupDnsConfig(QWidget* widget, QVBoxLayout* layout) {
    // Row 1: Hostname + Record type
    auto* queryRow = new QHBoxLayout();
    queryRow->addWidget(new QLabel(tr("Hostname:"), widget));
    m_dnsHostname = new QLineEdit(widget);
    m_dnsHostname->setPlaceholderText(tr("e.g. example.com"));
    m_dnsHostname->setToolTip(tr("Domain name or IP address to query"));
    setAccessible(m_dnsHostname, tr("DNS hostname"));
    queryRow->addWidget(m_dnsHostname, 2);

    queryRow->addWidget(new QLabel(tr("Type:"), widget));
    m_dnsRecordType = new QComboBox(widget);
    m_dnsRecordType->addItems({"A", "AAAA", "MX", "CNAME", "TXT", "NS", "SOA", "SRV", "PTR"});
    m_dnsRecordType->setToolTip(tr("DNS record type to query"));
    setAccessible(m_dnsRecordType,
                  tr("DNS record type"),
                  tr("Select the type of DNS record to look up"));
    queryRow->addWidget(m_dnsRecordType);
    layout->addLayout(queryRow);

    // Row 2: DNS server
    auto* serverRow = new QHBoxLayout();
    serverRow->addWidget(new QLabel(tr("Server:"), widget));
    m_dnsServer = new QComboBox(widget);
    m_dnsServer->setEditable(true);
    m_dnsServer->setToolTip(tr("DNS server to use for queries (or enter a custom IP)"));

    // Add well-known DNS servers (first entry is "System Default" with empty IP)
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    for (const auto& server : servers) {
        if (server.second.isEmpty()) {
            m_dnsServer->addItem(server.first, QString());
        } else {
            m_dnsServer->addItem(QStringLiteral("%1 (%2)").arg(server.first, server.second),
                                 server.second);
        }
    }
    setAccessible(m_dnsServer, tr("DNS server"));
    serverRow->addWidget(m_dnsServer, 1);
    serverRow->addStretch();
    layout->addLayout(serverRow);
}

void NetworkDiagnosticPanel::setupDnsControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btnRow = new QHBoxLayout();
    m_dnsQueryBtn = new QPushButton(tr("Query"), widget);
    m_dnsQueryBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_dnsQueryBtn->setToolTip(tr("Perform a DNS query for the specified hostname and record type"));
    setAccessible(m_dnsQueryBtn, tr("Run DNS query"));
    btnRow->addWidget(m_dnsQueryBtn);

    m_dnsReverseBtn = new QPushButton(tr("Reverse Lookup"), widget);
    m_dnsReverseBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_dnsReverseBtn->setToolTip(tr("Resolve IP address to hostname"));
    setAccessible(m_dnsReverseBtn, tr("Reverse DNS lookup"));
    btnRow->addWidget(m_dnsReverseBtn);

    m_dnsCompareBtn = new QPushButton(tr("Compare Servers"), widget);
    m_dnsCompareBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_dnsCompareBtn->setToolTip(tr("Query multiple DNS servers and compare results"));
    setAccessible(m_dnsCompareBtn, tr("Compare DNS servers"));
    btnRow->addWidget(m_dnsCompareBtn);

    m_dnsFlushBtn = new QPushButton(tr("Flush Cache"), widget);
    m_dnsFlushBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_dnsFlushBtn->setToolTip(tr("Flush the local DNS resolver cache (requires admin)"));
    setAccessible(m_dnsFlushBtn, tr("Flush DNS cache"));
    btnRow->addWidget(m_dnsFlushBtn);

    btnRow->addStretch();
    m_dnsStatusLabel = new QLabel(widget);
    m_dnsStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    btnRow->addWidget(m_dnsStatusLabel);
    layout->addLayout(btnRow);

    connect(m_dnsQueryBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsQuery);
    connect(
        m_dnsReverseBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsReverseLookup);
    connect(m_dnsCompareBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsCompare);
    connect(m_dnsFlushBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsFlushCache);
}

void NetworkDiagnosticPanel::setupDnsResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_dnsTable = new QTableWidget(widget);
    m_dnsTable->setColumnCount(5);
    m_dnsTable->setHorizontalHeaderLabels(
        {tr("Query"), tr("Type"), tr("Server"), tr("Response Time"), tr("Answers")});
    m_dnsTable->setAlternatingRowColors(true);
    m_dnsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dnsTable->verticalHeader()->setVisible(false);

    auto* dnsHeader = m_dnsTable->horizontalHeader();
    dnsHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    dnsHeader->resizeSection(0, 150);
    dnsHeader->setSectionResizeMode(1, QHeaderView::Interactive);
    dnsHeader->resizeSection(1, 60);
    dnsHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    dnsHeader->resizeSection(2, 120);
    dnsHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    dnsHeader->resizeSection(3, 100);
    dnsHeader->setSectionResizeMode(4, QHeaderView::Stretch);

    setAccessible(m_dnsTable, tr("DNS results"));
    m_dnsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_dnsTable, 1);
}

// -- Port Scan Tab --------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createPortScanTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupPortScanConfig(widget, layout);
    setupPortScanControls(widget, layout);
    setupPortScanResults(widget, layout);

    return widget;
}

void NetworkDiagnosticPanel::setupPortScanConfig(QWidget* widget, QVBoxLayout* layout) {
    auto* configRow1 = new QHBoxLayout();
    configRow1->addWidget(new QLabel(tr("Target:"), widget));
    m_portTarget = new QLineEdit(widget);
    m_portTarget->setPlaceholderText(tr("hostname or IP"));
    m_portTarget->setToolTip(tr("Target hostname or IP address to scan"));
    setAccessible(m_portTarget, tr("Port scan target"));
    configRow1->addWidget(m_portTarget, 2);

    configRow1->addWidget(new QLabel(tr("Preset:"), widget));
    m_portPreset = new QComboBox(widget);
    m_portPreset->addItem(tr("Custom"));
    const auto presets = PortScanner::getPresets();
    for (const auto& p : presets) {
        m_portPreset->addItem(p.name);
    }
    m_portPreset->setToolTip(tr("Select a predefined set of ports or choose Custom"));
    setAccessible(m_portPreset,
                  tr("Port preset"),
                  tr("Choose a common port set or specify custom ports"));
    configRow1->addWidget(m_portPreset);

    configRow1->addWidget(new QLabel(tr("Custom Ports:"), widget));
    m_portCustomRange = new QLineEdit(widget);
    m_portCustomRange->setPlaceholderText(tr("e.g. 80,443,8080-8090"));
    m_portCustomRange->setToolTip(tr("Comma-separated ports or ranges (e.g. 80,443,8080-8090)"));
    setAccessible(m_portCustomRange,
                  tr("Custom port range"),
                  tr("Specify individual ports or ranges separated by commas"));
    configRow1->addWidget(m_portCustomRange, 1);
    layout->addLayout(configRow1);

    connect(m_portPreset,
            &QComboBox::currentIndexChanged,
            this,
            &NetworkDiagnosticPanel::onPortPresetChanged);

    auto* configRow2 = new QHBoxLayout();
    configRow2->addWidget(new QLabel(tr("Timeout:"), widget));
    m_portTimeout = new QSpinBox(widget);
    m_portTimeout->setRange(100, 30'000);
    m_portTimeout->setValue(3000);
    m_portTimeout->setSuffix(tr(" ms"));
    m_portTimeout->setToolTip(tr("Connection timeout per port in milliseconds"));
    setAccessible(m_portTimeout,
                  tr("Port scan timeout"),
                  tr("Maximum wait time for each port connection attempt"));
    configRow2->addWidget(m_portTimeout);

    configRow2->addWidget(new QLabel(tr("Concurrent:"), widget));
    m_portConcurrent = new QSpinBox(widget);
    m_portConcurrent->setRange(1, 200);
    m_portConcurrent->setValue(50);
    m_portConcurrent->setToolTip(tr("Number of ports to scan simultaneously"));
    setAccessible(m_portConcurrent,
                  tr("Concurrent scans"),
                  tr("Maximum number of parallel port connections"));
    configRow2->addWidget(m_portConcurrent);

    m_portBannerGrab = new QCheckBox(tr("Banner Grab"), widget);
    m_portBannerGrab->setChecked(true);
    m_portBannerGrab->setToolTip(tr("Attempt to read service banners on open ports"));
    setAccessible(m_portBannerGrab,
                  tr("Banner grab"),
                  tr("Read service identification banners from open ports"));
    configRow2->addWidget(m_portBannerGrab);
    configRow2->addStretch();
    layout->addLayout(configRow2);
}

void NetworkDiagnosticPanel::setupPortScanControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btnRow = new QHBoxLayout();
    m_portStartBtn = new QPushButton(tr("Scan Ports"), widget);
    m_portStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_portStartBtn->setToolTip(tr("Begin scanning the specified ports on the target"));
    setAccessible(m_portStartBtn, tr("Start port scan"));
    btnRow->addWidget(m_portStartBtn);

    m_portStopBtn = new QPushButton(tr("Stop"), widget);
    m_portStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_portStopBtn->setEnabled(false);
    m_portStopBtn->setToolTip(tr("Cancel the current port scan"));
    setAccessible(m_portStopBtn, tr("Stop port scan"));
    btnRow->addWidget(m_portStopBtn);

    btnRow->addStretch();
    m_portSummaryLabel = new QLabel(widget);
    m_portSummaryLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    btnRow->addWidget(m_portSummaryLabel);
    layout->addLayout(btnRow);
}

void NetworkDiagnosticPanel::setupPortScanResults(QWidget* widget, QVBoxLayout* layout) {
    m_portProgress = new QProgressBar(widget);
    m_portProgress->setRange(0, 100);
    m_portProgress->setValue(0);
    m_portProgress->setVisible(false);
    setAccessible(m_portProgress,
                  tr("Port scan progress"),
                  tr("Progress of the current port scanning operation"));
    layout->addWidget(m_portProgress);

    m_portTable = new QTableWidget(widget);
    m_portTable->setColumnCount(5);
    m_portTable->setHorizontalHeaderLabels(
        {tr("Port"), tr("State"), tr("Service"), tr("Response (ms)"), tr("Banner")});
    m_portTable->setAlternatingRowColors(true);
    m_portTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_portTable->verticalHeader()->setVisible(false);
    m_portTable->setSortingEnabled(true);

    auto* portHeader = m_portTable->horizontalHeader();
    portHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    portHeader->resizeSection(0, 70);
    portHeader->setSectionResizeMode(1, QHeaderView::Interactive);
    portHeader->resizeSection(1, 80);
    portHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    portHeader->resizeSection(2, 120);
    portHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    portHeader->resizeSection(3, 100);
    portHeader->setSectionResizeMode(4, QHeaderView::Stretch);

    setAccessible(m_portTable, tr("Port scan results"));
    m_portTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_portTable, 1);
}

// -- Bandwidth Tab -------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createBandwidthTab() {
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* widget = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    auto* iperfGroup = new QGroupBox(tr("LAN Bandwidth (iPerf3)"), widget);
    auto* iperfLayout = new QVBoxLayout(iperfGroup);
    setupBandwidthIperfConfig(widget, iperfLayout);
    setupBandwidthIperfControls(widget, iperfLayout);
    setupBandwidthIperfResults(widget, iperfLayout);
    layout->addWidget(iperfGroup);

    setupBandwidthHttpSection(widget, layout);

    layout->addStretch();

    scrollArea->setWidget(widget);
    return scrollArea;
}

void NetworkDiagnosticPanel::setupBandwidthIperfConfig(QWidget* widget, QVBoxLayout* iperfLayout) {
    // Row 1: Server address + Port
    auto* serverRow = new QHBoxLayout();
    serverRow->addWidget(new QLabel(tr("Server:"), widget));
    m_bwServerAddr = new QLineEdit(widget);
    m_bwServerAddr->setPlaceholderText(tr("iPerf3 server address"));
    m_bwServerAddr->setToolTip(tr("Address of the iPerf3 server to test against"));
    setAccessible(m_bwServerAddr, tr("iPerf3 server address"));
    serverRow->addWidget(m_bwServerAddr, 2);

    serverRow->addWidget(new QLabel(tr("Port:"), widget));
    m_bwPort = new QSpinBox(widget);
    m_bwPort->setRange(1, 65'535);
    m_bwPort->setValue(5201);
    m_bwPort->setToolTip(tr("iPerf3 server port number"));
    setAccessible(m_bwPort, tr("iPerf3 port"), tr("TCP port for the iPerf3 server connection"));
    serverRow->addWidget(m_bwPort);
    iperfLayout->addLayout(serverRow);

    // Row 2: Duration + Streams + Bidirectional
    auto* optionsRow = new QHBoxLayout();
    optionsRow->addWidget(new QLabel(tr("Duration:"), widget));
    m_bwDuration = new QSpinBox(widget);
    m_bwDuration->setRange(1, 120);
    m_bwDuration->setValue(10);
    m_bwDuration->setSuffix(tr(" s"));
    m_bwDuration->setToolTip(tr("Duration of the bandwidth test in seconds"));
    setAccessible(m_bwDuration, tr("Test duration"), tr("How long to run the bandwidth test"));
    optionsRow->addWidget(m_bwDuration);

    optionsRow->addWidget(new QLabel(tr("Streams:"), widget));
    m_bwStreams = new QSpinBox(widget);
    m_bwStreams->setRange(1, 32);
    m_bwStreams->setValue(1);
    m_bwStreams->setToolTip(tr("Number of parallel streams for the test"));
    setAccessible(m_bwStreams,
                  tr("Parallel streams"),
                  tr("Number of simultaneous TCP connections for the test"));
    optionsRow->addWidget(m_bwStreams);

    m_bwBidirectional = new QCheckBox(tr("Bidirectional"), widget);
    m_bwBidirectional->setChecked(true);
    m_bwBidirectional->setToolTip(tr("Test both upload and download simultaneously"));
    setAccessible(m_bwBidirectional,
                  tr("Bidirectional test"),
                  tr("Run bandwidth test in both directions simultaneously"));
    optionsRow->addWidget(m_bwBidirectional);

    optionsRow->addStretch();
    iperfLayout->addLayout(optionsRow);
}

void NetworkDiagnosticPanel::setupBandwidthIperfControls(QWidget* widget,
                                                         QVBoxLayout* iperfLayout) {
    auto* iperfBtnRow = new QHBoxLayout();
    m_bwTestBtn = new QPushButton(tr("Run Test"), widget);
    m_bwTestBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_bwTestBtn->setToolTip(tr("Start bandwidth test against the iPerf3 server"));
    setAccessible(m_bwTestBtn, tr("Run iPerf3 bandwidth test"));
    iperfBtnRow->addWidget(m_bwTestBtn);

    m_bwServerStartBtn = new QPushButton(tr("Start Server"), widget);
    m_bwServerStartBtn->setStyleSheet(ui::kSuccessButtonStyle);
    m_bwServerStartBtn->setToolTip(
        tr("Start local iPerf3 server for other devices to test "
           "against"));
    setAccessible(m_bwServerStartBtn, tr("Start iPerf3 server"));
    iperfBtnRow->addWidget(m_bwServerStartBtn);

    m_bwServerStopBtn = new QPushButton(tr("Stop Server"), widget);
    m_bwServerStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_bwServerStopBtn->setEnabled(false);
    m_bwServerStopBtn->setToolTip(tr("Stop the local iPerf3 server"));
    setAccessible(m_bwServerStopBtn, tr("Stop iPerf3 server"));
    iperfBtnRow->addWidget(m_bwServerStopBtn);

    iperfBtnRow->addStretch();
    m_bwServerStatus = new QLabel(tr("Server: Stopped"), widget);
    m_bwServerStatus->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    iperfBtnRow->addWidget(m_bwServerStatus);
    iperfLayout->addLayout(iperfBtnRow);
}

void NetworkDiagnosticPanel::setupBandwidthIperfResults(QWidget* widget, QVBoxLayout* iperfLayout) {
    m_bwResultLabel = new QLabel(widget);
    m_bwResultLabel->setWordWrap(true);
    m_bwResultLabel->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(ui::kFontSizeStatus));
    iperfLayout->addWidget(m_bwResultLabel);
}

void NetworkDiagnosticPanel::setupBandwidthHttpSection(QWidget* widget, QVBoxLayout* layout) {
    auto* httpGroup = new QGroupBox(tr("Internet Speed (HTTP)"), widget);
    auto* httpLayout = new QVBoxLayout(httpGroup);
    auto* httpBtnRow = new QHBoxLayout();
    m_httpSpeedBtn = new QPushButton(tr("Run Speed Test"), widget);
    m_httpSpeedBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_httpSpeedBtn->setToolTip(tr("Download from public CDN servers to measure internet speed"));
    setAccessible(m_httpSpeedBtn, tr("HTTP speed test"));
    httpBtnRow->addWidget(m_httpSpeedBtn);
    httpBtnRow->addStretch();
    httpLayout->addLayout(httpBtnRow);

    connect(
        m_httpSpeedBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onRunHttpSpeedTest);

    m_httpSpeedLabel = new QLabel(widget);
    m_httpSpeedLabel->setWordWrap(true);
    m_httpSpeedLabel->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(ui::kFontSizeStatus));
    httpLayout->addWidget(m_httpSpeedLabel);
    layout->addWidget(httpGroup);
}

// -- WiFi Tab ------------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createWiFiTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);
    auto* btnRow = new QHBoxLayout();
    m_wifiScanBtn = new QPushButton(tr("Scan WiFi"), widget);
    m_wifiScanBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_wifiScanBtn->setToolTip(tr("Scan for available WiFi networks"));
    setAccessible(m_wifiScanBtn, tr("Scan WiFi networks"));
    btnRow->addWidget(m_wifiScanBtn);
    m_wifiContBtn = new QPushButton(tr("Continuous Scan"), widget);
    m_wifiContBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_wifiContBtn->setToolTip(tr("Start continuous WiFi scanning for real-time monitoring"));
    setAccessible(m_wifiContBtn, tr("Start continuous WiFi scan"));
    btnRow->addWidget(m_wifiContBtn);
    m_wifiStopBtn = new QPushButton(tr("Stop"), widget);
    m_wifiStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_wifiStopBtn->setEnabled(false);
    m_wifiStopBtn->setToolTip(tr("Stop continuous WiFi scanning"));
    setAccessible(m_wifiStopBtn, tr("Stop WiFi scan"));
    btnRow->addWidget(m_wifiStopBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    // Network table
    m_wifiTable = new QTableWidget(widget);
    m_wifiTable->setColumnCount(8);
    m_wifiTable->setHorizontalHeaderLabels({tr("SSID"),
                                            tr("BSSID"),
                                            tr("Signal"),
                                            tr("Quality"),
                                            tr("Channel"),
                                            tr("Band"),
                                            tr("Security"),
                                            tr("Vendor")});
    m_wifiTable->setAlternatingRowColors(true);
    m_wifiTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wifiTable->verticalHeader()->setVisible(false);
    m_wifiTable->setSortingEnabled(true);

    auto* wifiHeader = m_wifiTable->horizontalHeader();
    wifiHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    wifiHeader->setSectionResizeMode(1, QHeaderView::Interactive);
    wifiHeader->resizeSection(1, 130);
    wifiHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    wifiHeader->resizeSection(2, 75);
    wifiHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    wifiHeader->resizeSection(3, 75);
    wifiHeader->setSectionResizeMode(4, QHeaderView::Interactive);
    wifiHeader->resizeSection(4, 65);
    wifiHeader->setSectionResizeMode(5, QHeaderView::Interactive);
    wifiHeader->resizeSection(5, 60);
    wifiHeader->setSectionResizeMode(6, QHeaderView::Interactive);
    wifiHeader->resizeSection(6, 110);
    wifiHeader->setSectionResizeMode(7, QHeaderView::Interactive);
    wifiHeader->resizeSection(7, 100);

    setAccessible(m_wifiTable, tr("WiFi networks"));
    m_wifiTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_wifiTable, 1);

    // Channel utilization label
    m_wifiChannelLabel = new QLabel(widget);
    m_wifiChannelLabel->setWordWrap(true);
    m_wifiChannelLabel->setStyleSheet(QStringLiteral("color: %1; font-size: %2pt;")
                                          .arg(ui::kColorTextMuted)
                                          .arg(ui::kFontSizeSmall));
    layout->addWidget(m_wifiChannelLabel);

    return widget;
}

// -- Connections Tab -----------------------------------------------------

QWidget* NetworkDiagnosticPanel::createConnectionsTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupConnectionsConfig(widget, layout);
    setupConnectionsControls(widget, layout);
    setupConnectionsTable(widget, layout);

    return widget;
}

void NetworkDiagnosticPanel::setupConnectionsConfig(QWidget* widget, QVBoxLayout* layout) {
    auto* configRow = new QHBoxLayout();
    m_connShowTcp = new QCheckBox(tr("TCP"), widget);
    m_connShowTcp->setChecked(true);
    m_connShowTcp->setToolTip(tr("Show TCP connections"));
    setAccessible(m_connShowTcp, tr("Show TCP connections"));
    configRow->addWidget(m_connShowTcp);

    m_connShowUdp = new QCheckBox(tr("UDP"), widget);
    m_connShowUdp->setChecked(true);
    m_connShowUdp->setToolTip(tr("Show UDP connections"));
    setAccessible(m_connShowUdp, tr("Show UDP connections"));
    configRow->addWidget(m_connShowUdp);

    configRow->addWidget(new QLabel(tr("Process:"), widget));
    m_connProcessFilter = new QLineEdit(widget);
    m_connProcessFilter->setPlaceholderText(tr("filter by process name"));
    m_connProcessFilter->setClearButtonEnabled(true);
    m_connProcessFilter->setToolTip(tr("Filter connections by process name"));
    setAccessible(m_connProcessFilter, tr("Process filter"));
    configRow->addWidget(m_connProcessFilter, 1);

    configRow->addWidget(new QLabel(tr("Refresh:"), widget));
    m_connRefreshRate = new QSpinBox(widget);
    m_connRefreshRate->setRange(500, 30'000);
    m_connRefreshRate->setValue(2000);
    m_connRefreshRate->setSuffix(tr(" ms"));
    m_connRefreshRate->setToolTip(tr("How often to refresh the connection list"));
    setAccessible(m_connRefreshRate,
                  tr("Refresh rate"),
                  tr("Interval between connection list updates"));
    configRow->addWidget(m_connRefreshRate);
    layout->addLayout(configRow);
}

void NetworkDiagnosticPanel::setupConnectionsControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btnRow = new QHBoxLayout();
    m_connStartBtn = new QPushButton(tr("Start Monitor"), widget);
    m_connStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_connStartBtn->setToolTip(tr("Start monitoring active network connections"));
    setAccessible(m_connStartBtn, tr("Start connection monitor"));
    btnRow->addWidget(m_connStartBtn);

    m_connStopBtn = new QPushButton(tr("Stop"), widget);
    m_connStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_connStopBtn->setEnabled(false);
    m_connStopBtn->setToolTip(tr("Stop monitoring connections"));
    setAccessible(m_connStopBtn, tr("Stop connection monitor"));
    btnRow->addWidget(m_connStopBtn);

    btnRow->addStretch();
    m_connSummaryLabel = new QLabel(widget);
    m_connSummaryLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    btnRow->addWidget(m_connSummaryLabel);
    layout->addLayout(btnRow);
}

void NetworkDiagnosticPanel::setupConnectionsTable(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_connTable = new QTableWidget(widget);
    m_connTable->setColumnCount(7);
    m_connTable->setHorizontalHeaderLabels({tr("Protocol"),
                                            tr("Local Address"),
                                            tr("Local Port"),
                                            tr("Remote Address"),
                                            tr("Remote Port"),
                                            tr("State"),
                                            tr("Process")});
    m_connTable->setAlternatingRowColors(true);
    m_connTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_connTable->verticalHeader()->setVisible(false);
    m_connTable->setSortingEnabled(true);

    auto* connHeader = m_connTable->horizontalHeader();
    connHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    connHeader->resizeSection(0, 65);
    connHeader->setSectionResizeMode(1, QHeaderView::Interactive);
    connHeader->resizeSection(1, 120);
    connHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    connHeader->resizeSection(2, 75);
    connHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    connHeader->resizeSection(3, 120);
    connHeader->setSectionResizeMode(4, QHeaderView::Interactive);
    connHeader->resizeSection(4, 95);
    connHeader->setSectionResizeMode(5, QHeaderView::Interactive);
    connHeader->resizeSection(5, 105);
    connHeader->setSectionResizeMode(6, QHeaderView::Stretch);

    setAccessible(m_connTable, tr("Active connections"));
    m_connTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_connTable, 1);
}

// -- Firewall Tab --------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createFirewallTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    setupFirewallToolbar(widget, layout);
    setupFirewallRuleTable(widget, layout);
    setupFirewallAnalysis(widget, layout);

    return widget;
}

void NetworkDiagnosticPanel::setupFirewallToolbar(QWidget* widget, QVBoxLayout* layout) {
    auto* toolbar = new QHBoxLayout();
    m_fwAuditBtn = new QPushButton(tr("Full Audit"), widget);
    m_fwAuditBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_fwAuditBtn->setToolTip(
        tr("Enumerate all firewall rules, detect conflicts, and analyze gaps"));
    setAccessible(m_fwAuditBtn, tr("Run firewall audit"));
    toolbar->addWidget(m_fwAuditBtn);

    connect(m_fwAuditBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onAuditFirewall);

    toolbar->addSpacing(ui::kSpacingMedium);

    m_fwSearchBox = new QLineEdit(widget);
    m_fwSearchBox->setPlaceholderText(tr("Search rules..."));
    m_fwSearchBox->setClearButtonEnabled(true);
    m_fwSearchBox->setToolTip(tr("Filter firewall rules by name, port, or application"));
    setAccessible(m_fwSearchBox, tr("Search firewall rules"));
    toolbar->addWidget(m_fwSearchBox, 1);

    m_fwDirFilter = new QComboBox(widget);
    m_fwDirFilter->addItems({tr("All Directions"), tr("Inbound"), tr("Outbound")});
    m_fwDirFilter->setToolTip(tr("Filter rules by traffic direction"));
    setAccessible(m_fwDirFilter,
                  tr("Direction filter"),
                  tr("Show only inbound, outbound, or all firewall rules"));
    toolbar->addWidget(m_fwDirFilter);

    m_fwActionFilter = new QComboBox(widget);
    m_fwActionFilter->addItems({tr("All Actions"), tr("Allow"), tr("Block")});
    m_fwActionFilter->setToolTip(tr("Filter rules by action type"));
    setAccessible(m_fwActionFilter,
                  tr("Action filter"),
                  tr("Show only allow, block, or all firewall rules"));
    toolbar->addWidget(m_fwActionFilter);

    toolbar->addStretch();
    m_fwSummaryLabel = new QLabel(widget);
    m_fwSummaryLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    toolbar->addWidget(m_fwSummaryLabel);
    layout->addLayout(toolbar);
}

void NetworkDiagnosticPanel::setupFirewallRuleTable(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_fwRuleTable = new QTableWidget(widget);
    m_fwRuleTable->setColumnCount(8);
    m_fwRuleTable->setHorizontalHeaderLabels({tr("Enabled"),
                                              tr("Name"),
                                              tr("Direction"),
                                              tr("Action"),
                                              tr("Protocol"),
                                              tr("Local Ports"),
                                              tr("Remote Ports"),
                                              tr("Application")});
    m_fwRuleTable->setAlternatingRowColors(true);
    m_fwRuleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fwRuleTable->verticalHeader()->setVisible(false);
    m_fwRuleTable->setSortingEnabled(true);

    auto* fwHeader = m_fwRuleTable->horizontalHeader();
    fwHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    fwHeader->resizeSection(0, 60);
    fwHeader->setSectionResizeMode(1, QHeaderView::Stretch);
    fwHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    fwHeader->resizeSection(2, 80);
    fwHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    fwHeader->resizeSection(3, 60);
    fwHeader->setSectionResizeMode(4, QHeaderView::Interactive);
    fwHeader->resizeSection(4, 70);
    fwHeader->setSectionResizeMode(5, QHeaderView::Interactive);
    fwHeader->resizeSection(5, 100);
    fwHeader->setSectionResizeMode(6, QHeaderView::Interactive);
    fwHeader->resizeSection(6, 100);
    fwHeader->setSectionResizeMode(7, QHeaderView::Interactive);
    fwHeader->resizeSection(7, 150);

    setAccessible(m_fwRuleTable, tr("Firewall rules"));
    m_fwRuleTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_fwRuleTable, 2);
}

void NetworkDiagnosticPanel::setupFirewallAnalysis(QWidget* widget, QVBoxLayout* layout) {
    auto* analysisRow = new QHBoxLayout();

    auto* conflictGroup = new QGroupBox(tr("Conflicts"), widget);
    auto* conflictLayout = new QVBoxLayout(conflictGroup);
    m_fwConflictText = new QTextEdit(widget);
    m_fwConflictText->setReadOnly(true);
    m_fwConflictText->setMaximumHeight(120);
    setAccessible(m_fwConflictText,
                  tr("Firewall conflicts"),
                  tr("Detected firewall rule conflicts"));
    conflictLayout->addWidget(m_fwConflictText);
    analysisRow->addWidget(conflictGroup);

    auto* gapGroup = new QGroupBox(tr("Coverage Gaps"), widget);
    auto* gapLayout = new QVBoxLayout(gapGroup);
    m_fwGapText = new QTextEdit(widget);
    m_fwGapText->setReadOnly(true);
    m_fwGapText->setMaximumHeight(120);
    setAccessible(m_fwGapText, tr("Coverage gaps"), tr("Detected firewall coverage gaps"));
    gapLayout->addWidget(m_fwGapText);
    analysisRow->addWidget(gapGroup);

    layout->addLayout(analysisRow);
}

// -- Shares Tab ----------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createSharesTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    auto* configRow = new QHBoxLayout();
    configRow->addWidget(new QLabel(tr("Hostname:"), widget));
    m_shareHostname = new QLineEdit(widget);
    m_shareHostname->setPlaceholderText(tr("target hostname or IP (blank = local)"));
    m_shareHostname->setToolTip(tr("Leave blank to discover shares on the local machine"));
    setAccessible(m_shareHostname, tr("Share discovery hostname"));
    configRow->addWidget(m_shareHostname, 2);

    m_shareDiscoverBtn = new QPushButton(tr("Discover Shares"), widget);
    m_shareDiscoverBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_shareDiscoverBtn->setToolTip(tr("Discover shared folders and resources on the target host"));
    setAccessible(m_shareDiscoverBtn, tr("Discover network shares"));
    configRow->addWidget(m_shareDiscoverBtn);
    layout->addLayout(configRow);

    connect(
        m_shareDiscoverBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDiscoverShares);

    m_shareTable = new QTableWidget(widget);
    m_shareTable->setColumnCount(5);
    m_shareTable->setHorizontalHeaderLabels(
        {tr("Share Name"), tr("Type"), tr("Read"), tr("Write"), tr("Remark")});
    m_shareTable->setAlternatingRowColors(true);
    m_shareTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_shareTable->verticalHeader()->setVisible(false);

    auto* shareHeader = m_shareTable->horizontalHeader();
    shareHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    shareHeader->setSectionResizeMode(1, QHeaderView::Interactive);
    shareHeader->resizeSection(1, 80);
    shareHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    shareHeader->resizeSection(2, 60);
    shareHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    shareHeader->resizeSection(3, 60);
    shareHeader->setSectionResizeMode(4, QHeaderView::Stretch);

    setAccessible(m_shareTable, tr("Network shares"));
    m_shareTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_shareTable, 1);

    return widget;
}

// -- LAN Transfer Tab -- Group Builders ------------------------------------

QGroupBox* NetworkDiagnosticPanel::createLanServerGroup(QWidget* parent) {
    auto* group = new QGroupBox(tr("LAN Transfer Server (Receiver)"), parent);
    auto* group_layout = new QVBoxLayout(group);
    auto* row = new QHBoxLayout();

    row->addWidget(new QLabel(tr("Listen Port:"), parent));
    m_lanPort = new QSpinBox(parent);
    m_lanPort->setRange(1024, 65'535);
    m_lanPort->setValue(5050);
    m_lanPort->setToolTip(tr("TCP port for the LAN transfer server to listen on"));
    setAccessible(m_lanPort, tr("LAN transfer server port"));
    row->addWidget(m_lanPort);

    m_lanServerStartBtn = new QPushButton(tr("Start Server"), parent);
    m_lanServerStartBtn->setStyleSheet(ui::kSuccessButtonStyle);
    m_lanServerStartBtn->setToolTip(
        tr("Start a TCP server that receives data from the "
           "remote device to measure transfer speed"));
    setAccessible(m_lanServerStartBtn, tr("Start LAN transfer server"));
    row->addWidget(m_lanServerStartBtn);

    m_lanServerStopBtn = new QPushButton(tr("Stop Server"), parent);
    m_lanServerStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_lanServerStopBtn->setEnabled(false);
    m_lanServerStopBtn->setToolTip(tr("Stop the LAN transfer server"));
    setAccessible(m_lanServerStopBtn, tr("Stop LAN transfer server"));
    row->addWidget(m_lanServerStopBtn);

    row->addStretch();
    m_lanServerStatus = new QLabel(tr("Server: Stopped"), parent);
    m_lanServerStatus->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    row->addWidget(m_lanServerStatus);
    group_layout->addLayout(row);

    return group;
}

QGroupBox* NetworkDiagnosticPanel::createLanClientGroup(QWidget* parent) {
    auto* group = new QGroupBox(tr("LAN Transfer Client (Sender)"), parent);
    auto* group_layout = new QVBoxLayout(group);

    auto* targetRow = new QHBoxLayout();
    targetRow->addWidget(new QLabel(tr("Target:"), parent));
    m_lanTarget = new QLineEdit(parent);
    m_lanTarget->setPlaceholderText(tr("IP address of the receiving device"));
    m_lanTarget->setToolTip(
        tr("Enter the IP address of the device running "
           "the LAN Transfer server"));
    setAccessible(m_lanTarget, tr("LAN transfer target address"));
    targetRow->addWidget(m_lanTarget, 2);
    group_layout->addLayout(targetRow);

    auto* optRow = new QHBoxLayout();
    optRow->addWidget(new QLabel(tr("Duration:"), parent));
    m_lanDuration = new QSpinBox(parent);
    m_lanDuration->setRange(1, 120);
    m_lanDuration->setValue(10);
    m_lanDuration->setSuffix(tr(" s"));
    m_lanDuration->setToolTip(tr("How long to send data, in seconds"));
    setAccessible(m_lanDuration, tr("LAN transfer test duration"));
    optRow->addWidget(m_lanDuration);

    optRow->addWidget(new QLabel(tr("Block Size:"), parent));
    m_lanBlockSize = new QSpinBox(parent);
    m_lanBlockSize->setRange(1, 1024);
    m_lanBlockSize->setValue(64);
    m_lanBlockSize->setSuffix(tr(" KB"));
    m_lanBlockSize->setToolTip(
        tr("Size of each data block sent "
           "(larger may improve throughput)"));
    setAccessible(m_lanBlockSize, tr("LAN transfer block size"));
    optRow->addWidget(m_lanBlockSize);
    optRow->addStretch();
    group_layout->addLayout(optRow);

    auto* btnRow = new QHBoxLayout();
    m_lanTestBtn = new QPushButton(tr("Run Transfer Test"), parent);
    m_lanTestBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_lanTestBtn->setToolTip(
        tr("Send data to the target device and measure "
           "transfer speed"));
    setAccessible(m_lanTestBtn, tr("Run LAN transfer speed test"));
    btnRow->addWidget(m_lanTestBtn);
    btnRow->addStretch();
    group_layout->addLayout(btnRow);

    return group;
}

// -- LAN Transfer Tab ----------------------------------------------------

QWidget* NetworkDiagnosticPanel::createLanTransferTab() {
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* widget = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    layout->addWidget(createLanServerGroup(widget));
    layout->addWidget(createLanClientGroup(widget));

    m_lanResultLabel = new QLabel(widget);
    m_lanResultLabel->setWordWrap(true);
    m_lanResultLabel->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(ui::kFontSizeStatus));
    layout->addWidget(m_lanResultLabel);
    layout->addStretch();

    connect(m_lanServerStartBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onStartLanTransferServer);
    connect(m_lanServerStopBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onStopLanTransferServer);
    connect(
        m_lanTestBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onRunLanTransferTest);

    scrollArea->setWidget(widget);
    return scrollArea;
}

// -- Report Section ------------------------------------------------------


// ===================================================================
// Signal Connections
// ===================================================================

void NetworkDiagnosticPanel::connectSignals() {
    connectUiSignals();
    connectControllerCoreSignals();
    connectControllerAdapterPingTraceMtrSignals();
    connectControllerDnsPortSignals();
    connectControllerBandwidthSignals();
    connectControllerWifiConnectionsFirewallSharesSignals();
    connectFirewallFilterSignals();
}

void NetworkDiagnosticPanel::connectUiSignals() {
    // -- Adapter --
    connect(m_refreshBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onRefreshAdapters);
    connect(
        m_copyConfigBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onCopyAdapterConfig);
    connect(m_backupEthernetBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onBackupEthernetSettings);
    connect(m_restoreEthernetBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onRestoreEthernetSettings);
    connect(m_adapterTable,
            &QTableWidget::itemSelectionChanged,
            this,
            &NetworkDiagnosticPanel::onAdapterSelectionChanged);
    connect(m_adapterTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showAdapterContextMenu);

    // -- Ping --
    connect(m_pingStartBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStartPing);
    connect(m_pingStopBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStopPing);
    connect(m_pingTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showPingContextMenu);

    // -- Traceroute --
    connect(
        m_traceStartBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStartTraceroute);
    connect(m_traceStopBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStopTraceroute);
    connect(m_traceTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showTracerouteContextMenu);

    // -- MTR --
    connect(m_mtrStartBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStartMtr);
    connect(m_mtrStopBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStopMtr);
    connect(m_mtrTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showMtrContextMenu);

    // -- Port Scanner --
    connect(m_portStartBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStartPortScan);
    connect(m_portStopBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStopPortScan);
    connect(m_portTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showPortScanContextMenu);

    // -- Bandwidth --
    connect(
        m_bwTestBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStartBandwidthTest);
    connect(m_bwServerStartBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onStartIperfServer);
    connect(
        m_bwServerStopBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStopIperfServer);

    // -- WiFi --
    connect(m_wifiScanBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onScanWiFi);
    connect(
        m_wifiContBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStartContinuousWiFi);
    connect(
        m_wifiStopBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onStopContinuousWiFi);
    connect(m_wifiTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showWiFiContextMenu);

    // -- Connections --
    connect(m_connStartBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onStartConnectionMonitor);
    connect(m_connStopBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onStopConnectionMonitor);
    connect(m_connTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showConnectionsContextMenu);

    // -- DNS context menu --
    connect(m_dnsTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showDnsContextMenu);

    // -- Firewall context menu --
    connect(m_fwRuleTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showFirewallContextMenu);

    // -- Shares context menu --
    connect(m_shareTable,
            &QTableWidget::customContextMenuRequested,
            this,
            &NetworkDiagnosticPanel::showSharesContextMenu);
}

void NetworkDiagnosticPanel::connectControllerCoreSignals() {
    Q_ASSERT(m_controller);
    connect(m_controller.get(),
            &NetworkDiagnosticController::stateChanged,
            this,
            &NetworkDiagnosticPanel::onStateChanged);
    connect(m_controller.get(),
            &NetworkDiagnosticController::errorOccurred,
            this,
            &NetworkDiagnosticPanel::onError);
    connect(m_controller.get(),
            &NetworkDiagnosticController::statusMessage,
            this,
            &NetworkDiagnosticPanel::statusMessage);
    connect(m_controller.get(),
            &NetworkDiagnosticController::logOutput,
            this,
            &NetworkDiagnosticPanel::logOutput);
}

void NetworkDiagnosticPanel::connectControllerAdapterPingTraceMtrSignals() {
    Q_ASSERT(m_controller);
    connect(m_controller.get(),
            &NetworkDiagnosticController::adaptersScanComplete,
            this,
            &NetworkDiagnosticPanel::onAdaptersScanComplete);
    connect(m_controller.get(),
            &NetworkDiagnosticController::pingReplyReceived,
            this,
            &NetworkDiagnosticPanel::onPingReply);
    connect(m_controller.get(),
            &NetworkDiagnosticController::pingComplete,
            this,
            &NetworkDiagnosticPanel::onPingComplete);
    connect(m_controller.get(),
            &NetworkDiagnosticController::tracerouteHopReceived,
            this,
            &NetworkDiagnosticPanel::onTracerouteHop);
    connect(m_controller.get(),
            &NetworkDiagnosticController::tracerouteComplete,
            this,
            &NetworkDiagnosticPanel::onTracerouteComplete);
    connect(m_controller.get(),
            &NetworkDiagnosticController::mtrUpdateReceived,
            this,
            &NetworkDiagnosticPanel::onMtrUpdate);
    connect(m_controller.get(),
            &NetworkDiagnosticController::mtrComplete,
            this,
            &NetworkDiagnosticPanel::onMtrComplete);
}

void NetworkDiagnosticPanel::connectControllerDnsPortSignals() {
    Q_ASSERT(m_controller);
    connect(m_controller.get(),
            &NetworkDiagnosticController::dnsQueryComplete,
            this,
            &NetworkDiagnosticPanel::onDnsQueryComplete);
    connect(m_controller.get(),
            &NetworkDiagnosticController::dnsComparisonComplete,
            this,
            &NetworkDiagnosticPanel::onDnsComparisonComplete);
    connect(m_controller.get(), &NetworkDiagnosticController::dnsCacheFlushed, this, [this]() {
        Q_EMIT logOutput(tr("DNS cache flushed successfully"));
        Q_EMIT statusMessage(tr("DNS cache flushed"), 3000);
    });
    connect(m_controller.get(),
            &NetworkDiagnosticController::portScannedResult,
            this,
            &NetworkDiagnosticPanel::onPortScanned);
    connect(m_controller.get(),
            &NetworkDiagnosticController::portScanProgress,
            this,
            &NetworkDiagnosticPanel::onPortScanProgress);
    connect(m_controller.get(),
            &NetworkDiagnosticController::portScanComplete,
            this,
            &NetworkDiagnosticPanel::onPortScanComplete);
}

void NetworkDiagnosticPanel::connectControllerBandwidthSignals() {
    Q_ASSERT(m_bwResultLabel);
    Q_ASSERT(m_bwServerStatus);
    connect(m_controller.get(),
            &NetworkDiagnosticController::bandwidthComplete,
            this,
            &NetworkDiagnosticPanel::onBandwidthComplete);
    connect(m_controller.get(),
            &NetworkDiagnosticController::bandwidthProgress,
            this,
            [this](double currentMbps, double elapsedSec, double totalSec) {
                m_bwResultLabel->setText(QStringLiteral("Running: %1 Mbps (%2/%3 s)")
                                             .arg(currentMbps, 0, 'f', 1)
                                             .arg(elapsedSec, 0, 'f', 0)
                                             .arg(totalSec, 0, 'f', 0));
            });
    connect(m_controller.get(),
            &NetworkDiagnosticController::progressUpdated,
            this,
            [this](int percent, QString status) {
                Q_EMIT statusMessage(QStringLiteral("%1 (%2%)").arg(status).arg(percent), 2000);
                Q_EMIT progressUpdate(percent, 100);
            });
    connect(m_controller.get(),
            &NetworkDiagnosticController::httpSpeedComplete,
            this,
            &NetworkDiagnosticPanel::onHttpSpeedComplete);
    connect(m_controller.get(),
            &NetworkDiagnosticController::iperfServerStarted,
            this,
            [this](uint16_t port) {
                m_bwServerStatus->setText(QStringLiteral("Server: Running on port %1").arg(port));
                m_bwServerStartBtn->setEnabled(false);
                m_bwServerStopBtn->setEnabled(true);
            });
    connect(m_controller.get(), &NetworkDiagnosticController::iperfServerStopped, this, [this]() {
        Q_ASSERT(m_bwServerStatus);
        Q_ASSERT(m_bwServerStartBtn);
        m_bwServerStatus->setText(tr("Server: Stopped"));
        m_bwServerStartBtn->setEnabled(true);
        m_bwServerStopBtn->setEnabled(false);
    });
}

void NetworkDiagnosticPanel::connectControllerWifiConnectionsFirewallSharesSignals() {
    Q_ASSERT(m_wifiChannelLabel);
    Q_ASSERT(m_lanServerStatus);
    connect(m_controller.get(),
            &NetworkDiagnosticController::wifiScanComplete,
            this,
            &NetworkDiagnosticPanel::onWiFiScanComplete);
    connect(m_controller.get(),
            &NetworkDiagnosticController::wifiChannelUtilization,
            this,
            [this](QVector<WiFiChannelUtilization> channels) {
                QString text;
                for (const auto& ch : channels) {
                    if (!text.isEmpty()) {
                        text += QStringLiteral(" | ");
                    }
                    text += QStringLiteral("Ch %1: %2 networks, avg %3 dBm")
                                .arg(ch.channelNumber)
                                .arg(ch.networkCount)
                                .arg(ch.averageSignalDbm);
                }
                m_wifiChannelLabel->setText(text);
            });
    connect(m_controller.get(),
            &NetworkDiagnosticController::connectionsUpdated,
            this,
            &NetworkDiagnosticPanel::onConnectionsUpdated);
    connect(m_controller.get(),
            &NetworkDiagnosticController::firewallAuditComplete,
            this,
            &NetworkDiagnosticPanel::onFirewallAuditComplete);

    connect(m_controller.get(),
            &NetworkDiagnosticController::sharesDiscovered,
            this,
            &NetworkDiagnosticPanel::onSharesDiscovered);

    // -- LAN Transfer --
    connect(m_controller.get(),
            &NetworkDiagnosticController::lanTransferServerStarted,
            this,
            [this](uint16_t port) {
                m_lanServerStatus->setText(QStringLiteral("Server: Running on port %1").arg(port));
                m_lanServerStartBtn->setEnabled(false);
                m_lanServerStopBtn->setEnabled(true);
            });
    connect(
        m_controller.get(), &NetworkDiagnosticController::lanTransferServerStopped, this, [this]() {
            m_lanServerStatus->setText(tr("Server: Stopped"));
            m_lanServerStartBtn->setEnabled(true);
            m_lanServerStopBtn->setEnabled(false);
        });
    connect(m_controller.get(),
            &NetworkDiagnosticController::lanTransferProgress,
            this,
            &NetworkDiagnosticPanel::onLanTransferProgress);
    connect(m_controller.get(),
            &NetworkDiagnosticController::lanTransferComplete,
            this,
            &NetworkDiagnosticPanel::onLanTransferComplete);

    connectReportAndEthernetSignals();
}

void NetworkDiagnosticPanel::connectReportAndEthernetSignals() {
    Q_ASSERT(m_controller);
    connect(m_controller.get(),
            &NetworkDiagnosticController::reportGenerated,
            this,
            [this](QString path) {
                Q_EMIT statusMessage(QStringLiteral("Report saved to %1").arg(path), 5000);
                QMessageBox::information(this,
                                         tr("Report Generated"),
                                         QStringLiteral("Report saved to:\n%1").arg(path));
            });
    connect(m_controller.get(),
            &NetworkDiagnosticController::ethernetBackupComplete,
            this,
            [this](QString path) {
                QMessageBox::information(this,
                                         tr("Backup Complete"),
                                         tr("Ethernet settings backed up to:\n%1\n\n"
                                            "This file can be used to restore settings on this "
                                            "or another PC.")
                                             .arg(path));
            });
    connect(m_controller.get(),
            &NetworkDiagnosticController::ethernetRestoreComplete,
            this,
            [this](bool success) {
                if (success) {
                    QMessageBox::information(this,
                                             tr("Restore Complete"),
                                             tr("Ethernet settings restored successfully.\n\n"
                                                "The adapter may take a moment to apply the new "
                                                "configuration."));
                } else {
                    sak::logWarning("Ethernet settings restore incomplete");
                    QMessageBox::warning(this,
                                         tr("Restore Incomplete"),
                                         tr("Some settings could not be restored.\n"
                                            "Check the log for details. Administrator "
                                            "privileges may be required."));
                }
                // Refresh adapters to show updated config
                m_controller->scanAdapters();
            });
}

void NetworkDiagnosticPanel::connectFirewallFilterSignals() {
    connect(m_fwSearchBox, &QLineEdit::textChanged, this, [this]() { filterFirewallRules(); });
    connect(m_fwDirFilter, &QComboBox::currentIndexChanged, this, [this]() {
        filterFirewallRules();
    });
    connect(m_fwActionFilter, &QComboBox::currentIndexChanged, this, [this]() {
        filterFirewallRules();
    });
}

// ===================================================================
// Slot Implementations
// ===================================================================

// -- Adapters --

void NetworkDiagnosticPanel::onRefreshAdapters() {
    Q_ASSERT(m_controller);
    m_controller->scanAdapters();
}

void NetworkDiagnosticPanel::onAdaptersScanComplete(QVector<NetworkAdapterInfo> adapters) {
    Q_ASSERT(m_adapterTable);
    m_adapters = adapters;
    m_adapterTable->setRowCount(0);
    m_adapterTable->setSortingEnabled(false);

    for (const auto& a : adapters) {
        const int row = m_adapterTable->rowCount();
        m_adapterTable->insertRow(row);

        auto* nameItem = new QTableWidgetItem(a.name);
        nameItem->setData(Qt::UserRole, row);  // Store original data index
        m_adapterTable->setItem(row, 0, nameItem);
        m_adapterTable->setItem(row, 1, new QTableWidgetItem(a.adapterType));

        auto* statusItem = new QTableWidgetItem(a.isConnected ? tr("Connected")
                                                              : tr("Disconnected"));
        statusItem->setForeground(a.isConnected ? QColor(ui::kColorSuccess)
                                                : QColor(ui::kColorError));
        m_adapterTable->setItem(row, 2, statusItem);

        const auto ip = a.ipv4Addresses.isEmpty() ? QStringLiteral("--") : a.ipv4Addresses.first();
        m_adapterTable->setItem(row, 3, new QTableWidgetItem(ip));
        m_adapterTable->setItem(row, 4, new QTableWidgetItem(a.macAddress));

        const auto speed = a.linkSpeedBps > 0
                               ? QStringLiteral("%1 Mbps").arg(a.linkSpeedBps / 1'000'000)
                               : QStringLiteral("--");
        m_adapterTable->setItem(row, 5, new QTableWidgetItem(speed));
    }

    m_adapterTable->setSortingEnabled(true);
    Q_EMIT statusMessage(QStringLiteral("%1 adapters found").arg(adapters.size()), 3000);
}

QString NetworkDiagnosticPanel::formatAdapterIdentity(const NetworkAdapterInfo& adapter) const {
    return QStringLiteral("<b>%1</b><br>%2<br>MAC: %3")
        .arg(adapter.name, adapter.description, adapter.macAddress);
}

QString NetworkDiagnosticPanel::formatAdapterAddressing(const NetworkAdapterInfo& adapter) const {
    QString text;
    if (!adapter.ipv4Addresses.isEmpty()) {
        text +=
            QStringLiteral("IPv4: %1<br>").arg(adapter.ipv4Addresses.join(QStringLiteral(", ")));
    }
    if (!adapter.ipv6Addresses.isEmpty()) {
        text += QStringLiteral("IPv6: %1").arg(adapter.ipv6Addresses.join(QStringLiteral(", ")));
    }
    return text;
}

QString NetworkDiagnosticPanel::formatAdapterGatewayDns(const NetworkAdapterInfo& adapter) const {
    QString text;
    QStringList gateways;
    if (!adapter.ipv4Gateway.isEmpty()) {
        gateways << adapter.ipv4Gateway;
    }
    if (!adapter.ipv6Gateway.isEmpty()) {
        gateways << adapter.ipv6Gateway;
    }
    if (!gateways.isEmpty()) {
        text += QStringLiteral("GW: %1<br>").arg(gateways.join(QStringLiteral(", ")));
    }
    QStringList dns;
    dns << adapter.ipv4DnsServers << adapter.ipv6DnsServers;
    if (!dns.isEmpty()) {
        text += QStringLiteral("DNS: %1").arg(dns.join(QStringLiteral(", ")));
    }
    return text;
}

QString NetworkDiagnosticPanel::formatAdapterStatus(const NetworkAdapterInfo& adapter) const {
    QString text;
    text +=
        QStringLiteral("DHCP: %1<br>").arg(adapter.dhcpEnabled ? tr("Enabled") : tr("Disabled"));
    if (adapter.linkSpeedBps > 0) {
        text += QStringLiteral("Speed: %1 Mbps").arg(adapter.linkSpeedBps / 1'000'000);
    }
    return text;
}

void NetworkDiagnosticPanel::onAdapterSelectionChanged() {
    Q_ASSERT(m_adapterTable);
    Q_ASSERT(m_copyConfigBtn);
    const int row = m_adapterTable->currentRow();
    m_copyConfigBtn->setEnabled(row >= 0);
    m_backupEthernetBtn->setEnabled(row >= 0);

    if (row < 0) {
        m_detailIdentity->setText(tr("Select an adapter to view details"));
        m_detailAddressing->clear();
        m_detailGatewayDns->clear();
        m_detailStatus->clear();
        return;
    }

    const auto* name_item = m_adapterTable->item(row, 0);
    if (name_item == nullptr) {
        return;
    }
    const int data_idx = name_item->data(Qt::UserRole).toInt();
    if (data_idx < 0 || data_idx >= m_adapters.size()) {
        return;
    }

    const auto& adapter = m_adapters[data_idx];
    m_detailIdentity->setText(formatAdapterIdentity(adapter));
    m_detailAddressing->setText(formatAdapterAddressing(adapter));
    m_detailGatewayDns->setText(formatAdapterGatewayDns(adapter));
    m_detailStatus->setText(formatAdapterStatus(adapter));
}

void NetworkDiagnosticPanel::onCopyAdapterConfig() {
    Q_ASSERT(m_adapterTable);
    const int row = m_adapterTable->currentRow();
    if (row < 0) {
        return;
    }

    const auto* nameItem = m_adapterTable->item(row, 0);
    if (nameItem == nullptr) {
        return;
    }
    const int dataIdx = nameItem->data(Qt::UserRole).toInt();
    if (dataIdx < 0 || dataIdx >= m_adapters.size()) {
        return;
    }

    const auto& a = m_adapters[dataIdx];
    QString config;
    config += QStringLiteral("Name: %1\n").arg(a.name);
    config += QStringLiteral("Type: %1\n").arg(a.adapterType);
    config += QStringLiteral("MAC: %1\n").arg(a.macAddress);
    config += QStringLiteral("Status: %1\n").arg(a.isConnected ? "Connected" : "Disconnected");
    config += QStringLiteral("DHCP: %1\n").arg(a.dhcpEnabled ? "Enabled" : "Disabled");
    config += QStringLiteral("IPv4: %1\n").arg(a.ipv4Addresses.join(", "));
    config += QStringLiteral("IPv6: %1\n").arg(a.ipv6Addresses.join(", "));
    {
        QStringList gw;
        if (!a.ipv4Gateway.isEmpty()) {
            gw << a.ipv4Gateway;
        }
        if (!a.ipv6Gateway.isEmpty()) {
            gw << a.ipv6Gateway;
        }
        config += QStringLiteral("Gateways: %1\n").arg(gw.join(", "));
    }
    {
        QStringList dns;
        dns << a.ipv4DnsServers << a.ipv6DnsServers;
        config += QStringLiteral("DNS: %1\n").arg(dns.join(", "));
    }
    config += QStringLiteral("Speed: %1 Mbps\n").arg(a.linkSpeedBps / 1'000'000);

    QApplication::clipboard()->setText(config);
    Q_EMIT statusMessage(tr("Adapter configuration copied to clipboard"), 3000);
}

void NetworkDiagnosticPanel::onBackupEthernetSettings() {
    Q_ASSERT(m_adapterTable);
    Q_ASSERT(m_controller);
    const int row = m_adapterTable->currentRow();
    if (row < 0) {
        return;
    }

    const auto* nameItem = m_adapterTable->item(row, 0);
    if (nameItem == nullptr) {
        return;
    }
    const int dataIdx = nameItem->data(Qt::UserRole).toInt();
    if (dataIdx < 0 || dataIdx >= m_adapters.size()) {
        return;
    }

    const auto& adapter = m_adapters[dataIdx];

    QString filePath =
        QFileDialog::getSaveFileName(this,
                                     tr("Save Ethernet Settings Backup"),
                                     QStringLiteral("%1_ethernet_backup.json").arg(adapter.name),
                                     tr("JSON Files (*.json);;All Files (*.*)"));

    if (filePath.isEmpty()) {
        return;
    }

    m_controller->backupEthernetSettings(adapter.name, filePath);
}

void NetworkDiagnosticPanel::onRestoreEthernetSettings() {
    Q_ASSERT(m_controller);
    QString filePath = QFileDialog::getOpenFileName(this,
                                                    tr("Open Ethernet Settings Backup"),
                                                    QString(),
                                                    tr("JSON Files (*.json);;All Files (*.*)"));

    if (filePath.isEmpty()) {
        return;
    }

    // Let user choose target adapter
    QStringList adapters = m_controller->listEthernetAdapters();
    if (adapters.isEmpty()) {
        sak::logWarning("No Ethernet adapters found for settings restore");
        QMessageBox::warning(this,
                             tr("No Adapters"),
                             tr("No Ethernet adapters found on this system."));
        return;
    }

    // If there's only one adapter, use it directly
    QString targetAdapter;
    if (adapters.size() == 1) {
        targetAdapter = adapters.first();
    } else {
        // Show selection dialog
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Select Target Adapter"));
        dialog.setMinimumWidth(350);

        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(
            tr("Select the Ethernet adapter to apply the backup settings to:"), &dialog));

        auto* adapterCombo = new QComboBox(&dialog);
        for (const auto& name : adapters) {
            adapterCombo->addItem(name);
        }
        layout->addWidget(adapterCombo);

        auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                               &dialog);
        layout->addWidget(buttonBox);

        connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        targetAdapter = adapterCombo->currentText();
    }

    auto result =
        QMessageBox::warning(this,
                             tr("Confirm Restore"),
                             tr("This will change the IP configuration of <b>%1</b>.\n\n"
                                "Current settings will be overwritten. You may lose network "
                                "connectivity temporarily.\n\n"
                                "Administrator privileges are required.\n\n"
                                "Continue?")
                                 .arg(targetAdapter),
                             QMessageBox::Yes | QMessageBox::No,
                             QMessageBox::No);

    if (result == QMessageBox::Yes) {
        m_controller->restoreEthernetSettings(filePath, targetAdapter);
    }
}

// -- Adapter Context Menu ------------------------------------------------

const NetworkAdapterInfo* NetworkDiagnosticPanel::selectedAdapter() const {
    Q_ASSERT(m_adapterTable);
    const int row = m_adapterTable->currentRow();
    if (row < 0) {
        return nullptr;
    }
    const auto* name_item = m_adapterTable->item(row, 0);
    if (!name_item) {
        return nullptr;
    }
    const int data_idx = name_item->data(Qt::UserRole).toInt();
    if (data_idx < 0 || data_idx >= m_adapters.size()) {
        return nullptr;
    }
    return &m_adapters[data_idx];
}

QVector<const NetworkAdapterInfo*> NetworkDiagnosticPanel::selectedAdapters() const {
    Q_ASSERT(m_adapterTable);
    QVector<const NetworkAdapterInfo*> result;
    const auto selected_rows = m_adapterTable->selectionModel()->selectedRows();
    for (const auto& index : selected_rows) {
        const auto* name_item = m_adapterTable->item(index.row(), 0);
        if (!name_item) {
            continue;
        }
        const int data_idx = name_item->data(Qt::UserRole).toInt();
        if (data_idx >= 0 && data_idx < m_adapters.size()) {
            result.append(&m_adapters[data_idx]);
        }
    }
    return result;
}

bool NetworkDiagnosticPanel::runNetshCommand(const QStringList& args, QString* output) {
    QProcess process;
    process.setProgram(QStringLiteral("netsh"));
    process.setArguments(args);
    process.start();

    constexpr int kStartTimeoutMs = 5000;
    constexpr int kFinishTimeoutMs = 15'000;

    if (!process.waitForStarted(kStartTimeoutMs)) {
        if (output) {
            *output = tr("Failed to start netsh process");
        }
        return false;
    }
    if (!process.waitForFinished(kFinishTimeoutMs)) {
        process.kill();
        if (output) {
            *output = tr("netsh command timed out");
        }
        return false;
    }

    const QString stdout_text = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString stderr_text = QString::fromLocal8Bit(process.readAllStandardError());
    if (output) {
        *output = stdout_text.isEmpty() ? stderr_text : stdout_text;
    }
    return process.exitCode() == 0;
}

namespace {
bool areBridgeable(const QVector<const NetworkAdapterInfo*>& adapters) {
    if (adapters.size() <= 1) {
        return false;
    }
    for (const auto* sel : adapters) {
        const auto& sel_type = sel->adapterType;
        if (sel_type == QStringLiteral("Loopback") || sel_type == QStringLiteral("VPN")) {
            return false;
        }
    }
    return true;
}
}  // namespace

void NetworkDiagnosticPanel::addTypeSpecificMenuItems(
    QMenu& menu,
    const NetworkAdapterInfo& adapter,
    const QVector<const NetworkAdapterInfo*>& selected) {
    const auto& type = adapter.adapterType;
    const bool is_loopback = (type == QStringLiteral("Loopback"));
    const bool is_vpn = (type == QStringLiteral("VPN"));
    const bool is_bluetooth = (type == QStringLiteral("Bluetooth"));
    const bool is_ethernet = (type == QStringLiteral("Ethernet"));
    const bool is_wifi = (type == QStringLiteral("WiFi"));
    const bool is_ip_configurable = static_cast<bool>(is_ethernet | is_wifi);
    const bool can_rename = static_cast<bool>(!is_loopback & !is_vpn);

    if (!is_loopback) {
        if (adapter.isConnected) {
            menu.addAction(tr("Disable"), this, &NetworkDiagnosticPanel::onAdapterDisable);
        } else {
            menu.addAction(tr("Enable"), this, &NetworkDiagnosticPanel::onAdapterEnable);
        }
        menu.addAction(tr("Diagnose"), this, &NetworkDiagnosticPanel::onAdapterDiagnose);
        menu.addSeparator();
    }

    if (is_bluetooth) {
        menu.addAction(tr("View Bluetooth Devices"),
                       this,
                       &NetworkDiagnosticPanel::onViewBluetoothDevices);
        menu.addSeparator();
    }

    if (is_ip_configurable) {
        buildIpConfigSubmenu(menu, adapter);
        menu.addSeparator();
    }

    if (can_rename) {
        menu.addAction(tr("Rename..."), this, &NetworkDiagnosticPanel::onAdapterRename);
    }

    menu.addAction(tr("Copy Configuration"), this, &NetworkDiagnosticPanel::onCopyAdapterConfig);
    menu.addSeparator();

    if (is_ethernet) {
        menu.addAction(tr("Backup Settings..."),
                       this,
                       &NetworkDiagnosticPanel::onBackupEthernetSettings);
        menu.addAction(tr("Restore Settings..."),
                       this,
                       &NetworkDiagnosticPanel::onRestoreEthernetSettings);
        menu.addSeparator();
    }

    if (areBridgeable(selected)) {
        menu.addAction(tr("Bridge Connections"),
                       this,
                       &NetworkDiagnosticPanel::onBridgeConnections);
        menu.addSeparator();
    }

    if (!is_loopback) {
        menu.addAction(tr("Open Adapter Settings"),
                       this,
                       &NetworkDiagnosticPanel::onOpenAdapterSettings);
    }
}

void NetworkDiagnosticPanel::showAdapterContextMenu(const QPoint& pos) {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    const auto selected = selectedAdapters();
    QMenu menu(this);

    menu.addAction(tr("Status..."), this, &NetworkDiagnosticPanel::onAdapterStatus);
    menu.addAction(tr("Properties"), this, &NetworkDiagnosticPanel::onAdapterProperties);
    menu.addSeparator();

    addTypeSpecificMenuItems(menu, *adapter, selected);

    menu.exec(m_adapterTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::buildIpConfigSubmenu(QMenu& parent,
                                                  const NetworkAdapterInfo& adapter) {
    auto* ip_menu = parent.addMenu(tr("IP Configuration"));
    ip_menu->addAction(tr("Set Static IP..."), this, &NetworkDiagnosticPanel::onSetStaticIp);
    ip_menu->addAction(tr("Set DNS Servers..."), this, &NetworkDiagnosticPanel::onSetDnsServers);

    if (!adapter.dhcpEnabled) {
        ip_menu->addAction(tr("Enable DHCP"), this, &NetworkDiagnosticPanel::onEnableDhcp);
    }

    ip_menu->addSeparator();

    if (adapter.dhcpEnabled) {
        ip_menu->addAction(tr("Release DHCP Lease"),
                           this,
                           &NetworkDiagnosticPanel::onReleaseDhcpLease);
        ip_menu->addAction(tr("Renew DHCP Lease"), this, &NetworkDiagnosticPanel::onRenewDhcpLease);
    }
}

// -- Adapter Status Dialog -----------------------------------------------

void NetworkDiagnosticPanel::addStatusCategory(QTreeWidget* tree,
                                               const QString& category,
                                               const QVector<QPair<QString, QString>>& items) {
    auto* node = new QTreeWidgetItem(tree);
    node->setText(0, category);
    node->setFlags(node->flags() & ~Qt::ItemIsSelectable);
    auto font = node->font(0);
    font.setBold(true);
    node->setFont(0, font);

    for (const auto& [key, value] : items) {
        if (!value.isEmpty()) {
            auto* child = new QTreeWidgetItem(node);
            child->setText(0, key);
            child->setText(1, value);
        }
    }
}

void NetworkDiagnosticPanel::populateStatusTree(QTreeWidget* tree,
                                                const NetworkAdapterInfo& adapter) {
    addStatusCategory(tree,
                      tr("General"),
                      {{tr("Name"), adapter.name},
                       {tr("Description"), adapter.description},
                       {tr("Type"), adapter.adapterType},
                       {tr("MAC Address"), adapter.macAddress},
                       {tr("Interface Index"), QString::number(adapter.interfaceIndex)}});

    const auto speed_text = adapter.linkSpeedBps > 0
                                ? QStringLiteral("%1 Mbps").arg(adapter.linkSpeedBps / 1'000'000)
                                : tr("N/A");
    addStatusCategory(tree,
                      tr("Connection"),
                      {{tr("Status"), adapter.isConnected ? tr("Connected") : tr("Disconnected")},
                       {tr("Media State"), adapter.mediaState},
                       {tr("Link Speed"), speed_text}});

    populateStatusIpv4(tree, adapter);
    populateStatusIpv6(tree, adapter);

    addStatusCategory(tree,
                      tr("DHCP"),
                      {{tr("Enabled"), adapter.dhcpEnabled ? tr("Yes") : tr("No")},
                       {tr("Server"), adapter.dhcpServer},
                       {tr("Lease Obtained"),
                        adapter.dhcpLeaseObtained.isValid()
                            ? QLocale().toString(adapter.dhcpLeaseObtained, QLocale::LongFormat)
                            : tr("N/A")},
                       {tr("Lease Expires"),
                        adapter.dhcpLeaseExpires.isValid()
                            ? QLocale().toString(adapter.dhcpLeaseExpires, QLocale::LongFormat)
                            : tr("N/A")}});

    populateStatusStatistics(tree, adapter);

    addStatusCategory(tree,
                      tr("Driver Information"),
                      {{tr("Driver Name"), adapter.driverName},
                       {tr("Driver Version"), adapter.driverVersion},
                       {tr("Driver Date"), adapter.driverDate}});
}

void NetworkDiagnosticPanel::populateStatusIpv4(QTreeWidget* tree,
                                                const NetworkAdapterInfo& adapter) {
    QVector<QPair<QString, QString>> items;
    for (int i = 0; i < adapter.ipv4Addresses.size(); ++i) {
        const auto mask = i < adapter.ipv4SubnetMasks.size() ? adapter.ipv4SubnetMasks[i]
                                                             : QString();
        const auto addr_text = mask.isEmpty()
                                   ? adapter.ipv4Addresses[i]
                                   : QStringLiteral("%1 / %2").arg(adapter.ipv4Addresses[i], mask);
        items.append({tr("Address %1").arg(i + 1), addr_text});
    }
    items.append({tr("Gateway"), adapter.ipv4Gateway});
    for (int i = 0; i < adapter.ipv4DnsServers.size(); ++i) {
        items.append({tr("DNS Server %1").arg(i + 1), adapter.ipv4DnsServers[i]});
    }
    addStatusCategory(tree, tr("IPv4 Configuration"), items);
}

void NetworkDiagnosticPanel::populateStatusIpv6(QTreeWidget* tree,
                                                const NetworkAdapterInfo& adapter) {
    QVector<QPair<QString, QString>> items;
    for (int i = 0; i < adapter.ipv6Addresses.size(); ++i) {
        items.append({tr("Address %1").arg(i + 1), adapter.ipv6Addresses[i]});
    }
    items.append({tr("Gateway"), adapter.ipv6Gateway});
    for (int i = 0; i < adapter.ipv6DnsServers.size(); ++i) {
        items.append({tr("DNS Server %1").arg(i + 1), adapter.ipv6DnsServers[i]});
    }
    addStatusCategory(tree, tr("IPv6 Configuration"), items);
}

void NetworkDiagnosticPanel::populateStatusStatistics(QTreeWidget* tree,
                                                      const NetworkAdapterInfo& adapter) {
    const auto locale = QLocale();
    addStatusCategory(tree,
                      tr("Statistics"),
                      {{tr("Bytes Received"), locale.toString(adapter.bytesReceived)},
                       {tr("Bytes Sent"), locale.toString(adapter.bytesSent)},
                       {tr("Packets Received"), locale.toString(adapter.packetsReceived)},
                       {tr("Packets Sent"), locale.toString(adapter.packetsSent)},
                       {tr("Receive Errors"), QString::number(adapter.errorsReceived)},
                       {tr("Send Errors"), QString::number(adapter.errorsSent)}});
}

void NetworkDiagnosticPanel::onAdapterStatus() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    constexpr int kStatusDialogWidth = 520;
    constexpr int kStatusDialogHeight = 600;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Adapter Status \xe2\x80\x94 %1").arg(adapter->name));
    dialog.setMinimumSize(kStatusDialogWidth, kStatusDialogHeight);
    auto* layout = new QVBoxLayout(&dialog);

    auto* tree = new QTreeWidget(&dialog);
    tree->setHeaderLabels(QStringList{tr("Property"), tr("Value")});
    tree->setAlternatingRowColors(true);
    tree->setRootIsDecorated(true);

    populateStatusTree(tree, *adapter);
    tree->expandAll();
    tree->resizeColumnToContents(0);
    layout->addWidget(tree);

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(button_box);

    dialog.exec();
    Q_EMIT logOutput(tr("Viewed status for adapter '%1'").arg(adapter->name));
}

// -- Adapter Actions -----------------------------------------------------

void NetworkDiagnosticPanel::onAdapterProperties() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    // Open type-appropriate Windows Settings page
    const auto& type = adapter->adapterType;
    QString settings_uri;
    if (type == QStringLiteral("Ethernet")) {
        settings_uri = QStringLiteral("ms-settings:network-ethernet");
    } else if (type == QStringLiteral("WiFi")) {
        settings_uri = QStringLiteral("ms-settings:network-wifi");
    } else if (type == QStringLiteral("Bluetooth")) {
        settings_uri = QStringLiteral("ms-settings:bluetooth");
    } else if (type == QStringLiteral("VPN")) {
        settings_uri = QStringLiteral("ms-settings:network-vpn");
    } else {
        settings_uri = QStringLiteral("ms-settings:network");
    }

    QProcess::startDetached(QStringLiteral("explorer.exe"), {settings_uri});
    Q_EMIT logOutput(tr("Opened %1 properties for '%2'").arg(type, adapter->name));
    Q_EMIT statusMessage(tr("Opened %1 settings for '%2'").arg(type, adapter->name), 3000);
    sak::logInfo("Opened properties for {} adapter '{}'",
                 type.toStdString(),
                 adapter->name.toStdString());
}

void NetworkDiagnosticPanel::onAdapterEnable() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    Q_EMIT logOutput(tr("Enabling adapter '%1'...").arg(adapter->name));

    QString output;
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("set"),
                        QStringLiteral("interface"),
                        adapter->name,
                        QStringLiteral("admin=ENABLED")};

    if (runNetshCommand(args, &output)) {
        Q_EMIT statusMessage(tr("Adapter '%1' enabled").arg(adapter->name), 3000);
        Q_EMIT logOutput(tr("Adapter '%1' enabled successfully").arg(adapter->name));
        sak::logInfo("Enabled adapter: {}", adapter->name.toStdString());
    } else {
        sak::logError("Failed to enable adapter {}: {}",
                      adapter->name.toStdString(),
                      output.toStdString());
        Q_EMIT logOutput(tr("[ERROR] Failed to enable '%1': %2").arg(adapter->name, output));
        QMessageBox::warning(this,
                             tr("Enable Failed"),
                             tr("Failed to enable adapter.\n\n"
                                "Administrator privileges may be required.\n\n%1")
                                 .arg(output));
    }

    constexpr int kRefreshDelayMs = 2000;
    QTimer::singleShot(kRefreshDelayMs, this, &NetworkDiagnosticPanel::onRefreshAdapters);
}

void NetworkDiagnosticPanel::onAdapterDisable() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    auto confirm = QMessageBox::question(this,
                                         tr("Disable Adapter"),
                                         tr("Disable adapter <b>%1</b>?\n\n"
                                            "You may lose network connectivity.")
                                             .arg(adapter->name),
                                         QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    Q_EMIT logOutput(tr("Disabling adapter '%1'...").arg(adapter->name));

    QString output;
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("set"),
                        QStringLiteral("interface"),
                        adapter->name,
                        QStringLiteral("admin=DISABLED")};

    if (runNetshCommand(args, &output)) {
        Q_EMIT statusMessage(tr("Adapter '%1' disabled").arg(adapter->name), 3000);
        Q_EMIT logOutput(tr("Adapter '%1' disabled successfully").arg(adapter->name));
        sak::logInfo("Disabled adapter: {}", adapter->name.toStdString());
    } else {
        sak::logError("Failed to disable adapter {}: {}",
                      adapter->name.toStdString(),
                      output.toStdString());
        Q_EMIT logOutput(tr("[ERROR] Failed to disable '%1': %2").arg(adapter->name, output));
        QMessageBox::warning(this,
                             tr("Disable Failed"),
                             tr("Failed to disable adapter.\n\n"
                                "Administrator privileges may be required.\n\n%1")
                                 .arg(output));
    }

    constexpr int kRefreshDelayMs = 2000;
    QTimer::singleShot(kRefreshDelayMs, this, &NetworkDiagnosticPanel::onRefreshAdapters);
}

void NetworkDiagnosticPanel::onAdapterDiagnose() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    // Use the appropriate troubleshooter for the adapter type
    const auto& type = adapter->adapterType;
    QString diagnostic_id;
    if (type == QStringLiteral("Bluetooth")) {
        diagnostic_id = QStringLiteral("DeviceDiagnostic");
    } else if (type == QStringLiteral("VPN")) {
        diagnostic_id = QStringLiteral("NetworkDiagnosticsWeb");
    } else {
        diagnostic_id = QStringLiteral("NetworkDiagnosticsNetworkAdapter");
    }

    QProcess::startDetached(QStringLiteral("msdt.exe"), {QStringLiteral("/id"), diagnostic_id});
    Q_EMIT statusMessage(tr("Running %1 diagnostics for '%2'...").arg(type, adapter->name), 3000);
    Q_EMIT logOutput(tr("Launched %1 diagnostics for '%2'").arg(type, adapter->name));
    sak::logInfo("Launched diagnostics ({}) for adapter '{}'",
                 diagnostic_id.toStdString(),
                 adapter->name.toStdString());
}

void NetworkDiagnosticPanel::onAdapterRename() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    constexpr int kRenameDialogWidth = 400;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Rename Adapter"));
    dialog.setMinimumWidth(kRenameDialogWidth);
    auto* layout = new QVBoxLayout(&dialog);

    layout->addWidget(new QLabel(tr("Current name: <b>%1</b>").arg(adapter->name), &dialog));

    auto* name_edit = new QLineEdit(&dialog);
    name_edit->setText(adapter->name);
    name_edit->selectAll();
    name_edit->setPlaceholderText(tr("Enter new adapter name"));
    layout->addWidget(name_edit);

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            &dialog);
    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(button_box);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString new_name = name_edit->text().trimmed();
    if (new_name.isEmpty() || new_name == adapter->name) {
        return;
    }

    Q_EMIT logOutput(tr("Renaming adapter '%1' to '%2'...").arg(adapter->name, new_name));

    QString output;
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("set"),
                        QStringLiteral("interface"),
                        adapter->name,
                        QStringLiteral("newname=") + new_name};

    if (runNetshCommand(args, &output)) {
        Q_EMIT statusMessage(tr("Adapter renamed to '%1'").arg(new_name), 3000);
        Q_EMIT logOutput(tr("Adapter renamed to '%1'").arg(new_name));
        sak::logInfo("Renamed adapter '{}' to '{}'",
                     adapter->name.toStdString(),
                     new_name.toStdString());
        onRefreshAdapters();
    } else {
        sak::logError("Failed to rename adapter: {}", output.toStdString());
        Q_EMIT logOutput(tr("[ERROR] Failed to rename: %1").arg(output));
        QMessageBox::warning(this,
                             tr("Rename Failed"),
                             tr("Failed to rename adapter.\n\n"
                                "Administrator privileges may be required.\n\n%1")
                                 .arg(output));
    }
}

void NetworkDiagnosticPanel::onOpenAdapterSettings() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    const auto& type = adapter->adapterType;
    if (type == QStringLiteral("WiFi")) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("ms-settings:network-wifi")});
    } else if (type == QStringLiteral("Bluetooth")) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("ms-settings:bluetooth")});
    } else if (type == QStringLiteral("VPN")) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("ms-settings:network-vpn")});
    } else {
        QProcess::startDetached(QStringLiteral("control"), {QStringLiteral("ncpa.cpl")});
    }

    Q_EMIT logOutput(tr("Opened adapter settings for '%1'").arg(adapter->name));
    Q_EMIT statusMessage(tr("Opened adapter settings for '%1'").arg(adapter->name), 3000);
}

void NetworkDiagnosticPanel::onViewBluetoothDevices() {
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("ms-settings:bluetooth")});
    Q_EMIT logOutput(tr("Opened Bluetooth devices settings"));
    Q_EMIT statusMessage(tr("Opened Bluetooth devices settings"), 3000);
    sak::logInfo("Opened Bluetooth devices settings");
}

void NetworkDiagnosticPanel::onBridgeConnections() {
    const auto selected = selectedAdapters();
    if (selected.size() < 2) {
        QMessageBox::information(this,
                                 tr("Bridge Connections"),
                                 tr("Select two or more adapters to create a network bridge."));
        return;
    }

    QStringList adapter_names;
    for (const auto* sel : selected) {
        adapter_names << sel->name;
    }

    auto confirm = QMessageBox::question(this,
                                         tr("Bridge Connections"),
                                         tr("Bridge the following adapters?\n\n%1\n\n"
                                            "This will open Network Connections where you can "
                                            "select these adapters and bridge them.")
                                             .arg(adapter_names.join(QStringLiteral("\n"))),
                                         QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    QProcess::startDetached(QStringLiteral("control"), {QStringLiteral("ncpa.cpl")});
    Q_EMIT logOutput(tr("Bridge requested for: %1").arg(adapter_names.join(QStringLiteral(", "))));
    Q_EMIT statusMessage(
        tr("Opened Network Connections for bridging %1 adapters").arg(selected.size()), 5000);
    sak::logInfo("Bridge connection requested for {} adapters", std::to_string(selected.size()));
}

// -- IP Configuration Actions --------------------------------------------

void NetworkDiagnosticPanel::onSetStaticIp() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    constexpr int kIpDialogWidth = 420;
    const QString ip_pattern = QStringLiteral(
        "^((25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)\\.){3}"
        "(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)$");

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Set Static IP \xe2\x80\x94 %1").arg(adapter->name));
    dialog.setMinimumWidth(kIpDialogWidth);
    auto* layout = new QVBoxLayout(&dialog);

    auto* ip_validator = new QRegularExpressionValidator(QRegularExpression(ip_pattern), &dialog);

    auto make_ip_row = [&](const QString& label,
                           const QString& prefill,
                           const QString& placeholder) -> QLineEdit* {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, &dialog));
        auto* edit = new QLineEdit(&dialog);
        edit->setValidator(ip_validator);
        if (!prefill.isEmpty()) {
            edit->setText(prefill);
        }
        edit->setPlaceholderText(placeholder);
        row->addWidget(edit, 1);
        layout->addLayout(row);
        return edit;
    };

    const auto current_ip = adapter->ipv4Addresses.isEmpty() ? QString()
                                                             : adapter->ipv4Addresses.first();
    const auto current_mask = adapter->ipv4SubnetMasks.isEmpty() ? QString()
                                                                 : adapter->ipv4SubnetMasks.first();

    auto* ip_edit = make_ip_row(tr("IP Address:"), current_ip, QStringLiteral("192.168.1.100"));
    auto* mask_edit = make_ip_row(tr("Subnet Mask:"),
                                  current_mask.isEmpty() ? QStringLiteral("255.255.255.0")
                                                         : current_mask,
                                  QStringLiteral("255.255.255.0"));
    auto* gw_edit =
        make_ip_row(tr("Gateway:"), adapter->ipv4Gateway, QStringLiteral("192.168.1.1"));

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            &dialog);
    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(button_box);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString ip = ip_edit->text().trimmed();
    const QString mask = mask_edit->text().trimmed();
    const QString gateway = gw_edit->text().trimmed();
    if (ip.isEmpty() || mask.isEmpty()) {
        QMessageBox::warning(this,
                             tr("Invalid Input"),
                             tr("IP address and subnet mask are required."));
        return;
    }

    applyStaticIp(adapter->name, ip, mask, gateway);
}

void NetworkDiagnosticPanel::applyStaticIp(const QString& adapter_name,
                                           const QString& ip,
                                           const QString& mask,
                                           const QString& gateway) {
    QString output;
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("ipv4"),
                        QStringLiteral("set"),
                        QStringLiteral("address"),
                        adapter_name,
                        QStringLiteral("static"),
                        ip,
                        mask};
    if (!gateway.isEmpty()) {
        args << gateway;
    }

    Q_EMIT logOutput(
        tr("Setting static IP on '%1': %2 / %3 gw %4").arg(adapter_name, ip, mask, gateway));

    if (runNetshCommand(args, &output)) {
        Q_EMIT statusMessage(tr("Static IP configured on '%1'").arg(adapter_name), 3000);
        Q_EMIT logOutput(tr("Static IP configured on '%1'").arg(adapter_name));
        sak::logInfo("Static IP set on {}: {} / {} gw {}",
                     adapter_name.toStdString(),
                     ip.toStdString(),
                     mask.toStdString(),
                     gateway.toStdString());
        onRefreshAdapters();
    } else {
        sak::logError("Failed to set static IP: {}", output.toStdString());
        Q_EMIT logOutput(tr("[ERROR] Failed to set static IP: %1").arg(output));
        QMessageBox::warning(this,
                             tr("Failed to Set IP"),
                             tr("Failed to configure static IP.\n\n"
                                "Administrator privileges may be required.\n\n%1")
                                 .arg(output));
    }
}

void NetworkDiagnosticPanel::onSetDnsServers() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    constexpr int kDnsDialogWidth = 420;
    const QString ip_pattern = QStringLiteral(
        "^((25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)\\.){3}"
        "(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)$");

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Set DNS Servers \xe2\x80\x94 %1").arg(adapter->name));
    dialog.setMinimumWidth(kDnsDialogWidth);
    auto* layout = new QVBoxLayout(&dialog);

    auto* ip_validator = new QRegularExpressionValidator(QRegularExpression(ip_pattern), &dialog);

    const auto current_primary =
        adapter->ipv4DnsServers.isEmpty() ? QString() : adapter->ipv4DnsServers.first();
    const auto current_secondary = adapter->ipv4DnsServers.size() > 1 ? adapter->ipv4DnsServers[1]
                                                                      : QString();

    auto make_dns_row = [&](const QString& label,
                            const QString& prefill,
                            const QString& placeholder) -> QLineEdit* {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label, &dialog));
        auto* edit = new QLineEdit(&dialog);
        edit->setValidator(ip_validator);
        if (!prefill.isEmpty()) {
            edit->setText(prefill);
        }
        edit->setPlaceholderText(placeholder);
        row->addWidget(edit, 1);
        layout->addLayout(row);
        return edit;
    };

    auto* primary_edit =
        make_dns_row(tr("Primary DNS:"), current_primary, QStringLiteral("8.8.8.8"));
    auto* secondary_edit =
        make_dns_row(tr("Secondary DNS:"), current_secondary, QStringLiteral("8.8.4.4 (optional)"));

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            &dialog);
    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(button_box);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString primary = primary_edit->text().trimmed();
    const QString secondary = secondary_edit->text().trimmed();
    if (primary.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid Input"), tr("Primary DNS server is required."));
        return;
    }

    applyDnsServers(adapter->name, primary, secondary);
}

void NetworkDiagnosticPanel::applyDnsServers(const QString& adapter_name,
                                             const QString& primary,
                                             const QString& secondary) {
    QString output;

    // Set primary DNS
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("ipv4"),
                        QStringLiteral("set"),
                        QStringLiteral("dns"),
                        adapter_name,
                        QStringLiteral("static"),
                        primary};

    Q_EMIT logOutput(
        tr("Setting DNS on '%1': primary=%2 secondary=%3").arg(adapter_name, primary, secondary));

    if (!runNetshCommand(args, &output)) {
        sak::logError("Failed to set primary DNS: {}", output.toStdString());
        Q_EMIT logOutput(tr("[ERROR] Failed to set primary DNS: %1").arg(output));
        QMessageBox::warning(this,
                             tr("DNS Configuration Failed"),
                             tr("Failed to set primary DNS server.\n\n"
                                "Administrator privileges may be required.\n\n%1")
                                 .arg(output));
        return;
    }

    // Add secondary DNS if provided
    if (!secondary.isEmpty()) {
        QStringList add_args = {QStringLiteral("interface"),
                                QStringLiteral("ipv4"),
                                QStringLiteral("add"),
                                QStringLiteral("dns"),
                                adapter_name,
                                secondary,
                                QStringLiteral("index=2")};

        if (!runNetshCommand(add_args, &output)) {
            sak::logWarning("Failed to set secondary DNS: {}", output.toStdString());
        }
    }

    Q_EMIT statusMessage(tr("DNS servers configured on '%1'").arg(adapter_name), 3000);
    Q_EMIT logOutput(tr("DNS configured on '%1'").arg(adapter_name));
    sak::logInfo("DNS set on {}: primary={} secondary={}",
                 adapter_name.toStdString(),
                 primary.toStdString(),
                 secondary.toStdString());
    onRefreshAdapters();
}

void NetworkDiagnosticPanel::onEnableDhcp() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    auto confirm = QMessageBox::question(this,
                                         tr("Enable DHCP"),
                                         tr("Switch adapter <b>%1</b> to DHCP?\n\n"
                                            "The current static IP configuration will be removed.")
                                             .arg(adapter->name),
                                         QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    Q_EMIT logOutput(tr("Enabling DHCP on '%1'...").arg(adapter->name));

    QString output;
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("ipv4"),
                        QStringLiteral("set"),
                        QStringLiteral("address"),
                        adapter->name,
                        QStringLiteral("dhcp")};

    if (runNetshCommand(args, &output)) {
        // Also set DNS to DHCP
        QStringList dns_args = {QStringLiteral("interface"),
                                QStringLiteral("ipv4"),
                                QStringLiteral("set"),
                                QStringLiteral("dns"),
                                adapter->name,
                                QStringLiteral("dhcp")};
        runNetshCommand(dns_args);

        Q_EMIT statusMessage(tr("DHCP enabled on '%1'").arg(adapter->name), 3000);
        Q_EMIT logOutput(tr("DHCP enabled on '%1'").arg(adapter->name));
        sak::logInfo("DHCP enabled on: {}", adapter->name.toStdString());
        onRefreshAdapters();
    } else {
        sak::logError("Failed to enable DHCP: {}", output.toStdString());
        Q_EMIT logOutput(tr("[ERROR] Failed to enable DHCP: %1").arg(output));
        QMessageBox::warning(this,
                             tr("DHCP Failed"),
                             tr("Failed to enable DHCP.\n\n"
                                "Administrator privileges may be required.\n\n%1")
                                 .arg(output));
    }
}

void NetworkDiagnosticPanel::onReleaseDhcpLease() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    Q_EMIT logOutput(tr("Releasing DHCP lease on '%1'...").arg(adapter->name));

    QString output;
    QStringList args = {QStringLiteral("/release"), adapter->name};

    QProcess process;
    process.setProgram(QStringLiteral("ipconfig"));
    process.setArguments(args);
    process.start();

    constexpr int kIpconfigTimeoutMs = 10'000;
    process.waitForFinished(kIpconfigTimeoutMs);
    output = QString::fromLocal8Bit(process.readAllStandardOutput());

    if (process.exitCode() == 0) {
        Q_EMIT statusMessage(tr("DHCP lease released on '%1'").arg(adapter->name), 3000);
        Q_EMIT logOutput(tr("DHCP lease released on '%1'").arg(adapter->name));
        sak::logInfo("DHCP lease released: {}", adapter->name.toStdString());
    } else {
        sak::logWarning("DHCP release may have failed: {}", output.toStdString());
        Q_EMIT logOutput(tr("[WARN] DHCP release may have failed on '%1'").arg(adapter->name));
    }

    onRefreshAdapters();
}

void NetworkDiagnosticPanel::onRenewDhcpLease() {
    const auto* adapter = selectedAdapter();
    if (!adapter) {
        return;
    }

    Q_EMIT statusMessage(tr("Renewing DHCP lease on '%1'...").arg(adapter->name), 0);
    Q_EMIT logOutput(tr("Renewing DHCP lease on '%1'...").arg(adapter->name));

    QString output;
    QStringList args = {QStringLiteral("/renew"), adapter->name};

    QProcess process;
    process.setProgram(QStringLiteral("ipconfig"));
    process.setArguments(args);
    process.start();

    constexpr int kIpconfigTimeoutMs = 30'000;
    process.waitForFinished(kIpconfigTimeoutMs);
    output = QString::fromLocal8Bit(process.readAllStandardOutput());

    if (process.exitCode() == 0) {
        Q_EMIT statusMessage(tr("DHCP lease renewed on '%1'").arg(adapter->name), 3000);
        Q_EMIT logOutput(tr("DHCP lease renewed on '%1'").arg(adapter->name));
        sak::logInfo("DHCP lease renewed: {}", adapter->name.toStdString());
    } else {
        sak::logWarning("DHCP renew may have failed: {}", output.toStdString());
        Q_EMIT logOutput(tr("[WARN] DHCP renew may have failed on '%1'").arg(adapter->name));
    }

    onRefreshAdapters();
}

// -- Ping --

void NetworkDiagnosticPanel::onStartPing() {
    Q_ASSERT(m_pingTarget);
    Q_ASSERT(m_pingTable);
    const auto target = m_pingTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a ping target"), 3000);
        return;
    }

    m_pingTable->setRowCount(0);
    m_pingStatsLabel->clear();
    m_pingStartBtn->setEnabled(false);
    m_pingStopBtn->setEnabled(true);

    m_controller->ping({target,
                        m_pingCount->value(),
                        m_pingInterval->value(),
                        m_pingTimeout->value(),
                        m_pingPacketSize->value(),
                        128});  // default TTL
}

void NetworkDiagnosticPanel::onStopPing() {
    Q_ASSERT(m_controller);
    m_controller->cancel();
    m_pingStartBtn->setEnabled(true);
    m_pingStopBtn->setEnabled(false);
}

void NetworkDiagnosticPanel::onPingReply(PingReply reply) {
    Q_ASSERT(m_pingTable);
    const int row = m_pingTable->rowCount();
    m_pingTable->insertRow(row);

    m_pingTable->setItem(row, 0, new QTableWidgetItem(QString::number(reply.sequenceNumber)));
    m_pingTable->setItem(row, 1, new QTableWidgetItem(reply.replyFrom));

    auto* statusItem = new QTableWidgetItem(reply.success ? tr("Reply") : tr("Timeout"));
    statusItem->setForeground(reply.success ? QColor(ui::kColorSuccess) : QColor(ui::kColorError));
    m_pingTable->setItem(row, 2, statusItem);

    m_pingTable->setItem(row,
                         3,
                         new QTableWidgetItem(reply.success
                                                  ? QStringLiteral("%1").arg(reply.rttMs, 0, 'f', 1)
                                                  : QStringLiteral("--")));
    m_pingTable->setItem(row,
                         4,
                         new QTableWidgetItem(reply.success ? QString::number(reply.ttl)
                                                            : QStringLiteral("--")));

    m_pingTable->scrollToBottom();
}

void NetworkDiagnosticPanel::onPingComplete(PingResult result) {
    Q_ASSERT(m_pingStartBtn);
    Q_ASSERT(m_pingStopBtn);
    m_pingStartBtn->setEnabled(true);
    m_pingStopBtn->setEnabled(false);

    m_pingStatsLabel->setText(QStringLiteral("Sent: %1 | Rcvd: %2 | Lost: %3 (%4%) | "
                                             "Min: %5 ms | Max: %6 ms | Avg: %7 ms | Jitter: %8 ms")
                                  .arg(result.sent)
                                  .arg(result.received)
                                  .arg(result.lost)
                                  .arg(result.lossPercent, 0, 'f', 1)
                                  .arg(result.minRtt, 0, 'f', 1)
                                  .arg(result.maxRtt, 0, 'f', 1)
                                  .arg(result.avgRtt, 0, 'f', 1)
                                  .arg(result.jitter, 0, 'f', 2));

    Q_EMIT statusMessage(
        QStringLiteral("Ping complete -- %1% loss").arg(result.lossPercent, 0, 'f', 1), 5000);
}

// -- Traceroute --

void NetworkDiagnosticPanel::onStartTraceroute() {
    Q_ASSERT(m_traceTarget);
    Q_ASSERT(m_traceTable);
    const auto target = m_traceTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a traceroute target"), 3000);
        return;
    }

    m_traceTable->setRowCount(0);
    m_traceStatusLabel->clear();
    m_traceStartBtn->setEnabled(false);
    m_traceStopBtn->setEnabled(true);

    m_controller->traceroute(target, m_traceMaxHops->value(), 5000, 3, true);
}

void NetworkDiagnosticPanel::onStopTraceroute() {
    m_controller->cancel();
    m_traceStartBtn->setEnabled(true);
    m_traceStopBtn->setEnabled(false);
}

void NetworkDiagnosticPanel::onTracerouteHop(TracerouteHop hop) {
    Q_ASSERT(m_traceTable);
    Q_ASSERT(m_traceStatusLabel);
    const int row = m_traceTable->rowCount();
    m_traceTable->insertRow(row);

    m_traceTable->setItem(row, 0, new QTableWidgetItem(QString::number(hop.hopNumber)));

    if (hop.timedOut) {
        auto* timeoutItem = new QTableWidgetItem(tr("* * * Request timed out"));
        timeoutItem->setForeground(QColor(ui::kColorWarning));
        m_traceTable->setItem(row, 1, timeoutItem);
        for (int i = 2; i < 7; ++i) {
            m_traceTable->setItem(row, i, new QTableWidgetItem(QStringLiteral("*")));
        }
    } else {
        m_traceTable->setItem(row, 1, new QTableWidgetItem(hop.ipAddress));
        m_traceTable->setItem(row,
                              2,
                              new QTableWidgetItem(hop.hostname.isEmpty() ? QStringLiteral("--")
                                                                          : hop.hostname));
        m_traceTable->setItem(
            row, 3, new QTableWidgetItem(QStringLiteral("%1").arg(hop.rtt1Ms, 0, 'f', 1)));
        m_traceTable->setItem(
            row, 4, new QTableWidgetItem(QStringLiteral("%1").arg(hop.rtt2Ms, 0, 'f', 1)));
        m_traceTable->setItem(
            row, 5, new QTableWidgetItem(QStringLiteral("%1").arg(hop.rtt3Ms, 0, 'f', 1)));
        m_traceTable->setItem(
            row, 6, new QTableWidgetItem(QStringLiteral("%1").arg(hop.avgRttMs, 0, 'f', 1)));
    }

    m_traceStatusLabel->setText(QStringLiteral("Hop %1...").arg(hop.hopNumber));
    m_traceTable->scrollToBottom();
}

void NetworkDiagnosticPanel::onTracerouteComplete(TracerouteResult result) {
    Q_ASSERT(m_traceStartBtn);
    Q_ASSERT(m_traceStopBtn);
    m_traceStartBtn->setEnabled(true);
    m_traceStopBtn->setEnabled(false);

    QString status;
    if (result.reachedTarget) {
        status = QStringLiteral("Reached %1 in %2 hops").arg(result.target).arg(result.hops.size());
    } else {
        status = QStringLiteral("Could not reach %1 (%2 hops)")
                     .arg(result.target)
                     .arg(result.hops.size());
    }
    m_traceStatusLabel->setText(status);
    Q_EMIT statusMessage(status, 5000);
}

// -- MTR --

void NetworkDiagnosticPanel::onStartMtr() {
    Q_ASSERT(m_mtrTarget);
    Q_ASSERT(m_mtrTable);
    const auto target = m_mtrTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter an MTR target"), 3000);
        return;
    }

    m_mtrTable->setRowCount(0);
    m_mtrStatusLabel->clear();
    m_mtrStartBtn->setEnabled(false);
    m_mtrStopBtn->setEnabled(true);

    m_controller->mtr(target, m_mtrCycles->value(), 1000, 30, 5000);
}

void NetworkDiagnosticPanel::onStopMtr() {
    m_controller->cancel();
    m_mtrStartBtn->setEnabled(true);
    m_mtrStopBtn->setEnabled(false);
}

void NetworkDiagnosticPanel::onMtrUpdate(QVector<MtrHopStats> hops, int cycle) {
    Q_ASSERT(m_mtrTable);
    Q_ASSERT(m_mtrStatusLabel);
    m_mtrTable->setSortingEnabled(false);
    m_mtrTable->setRowCount(hops.size());

    for (int i = 0; i < hops.size(); ++i) {
        const auto& h = hops[i];

        // Reuse existing items to avoid allocation churn on frequent updates
        auto setOrCreate = [this](int row, int col, const QString& text) {
            if (auto* existing = m_mtrTable->item(row, col)) {
                existing->setText(text);
            } else {
                m_mtrTable->setItem(row, col, new QTableWidgetItem(text));
            }
        };

        setOrCreate(i, 0, QString::number(h.hopNumber));

        const auto label = h.hostname.isEmpty()
                               ? h.ipAddress
                               : QStringLiteral("%1 (%2)").arg(h.hostname, h.ipAddress);
        setOrCreate(i, 1, label);

        const auto lossText = QStringLiteral("%1").arg(h.lossPercent, 0, 'f', 1);
        if (auto* existing = m_mtrTable->item(i, 2)) {
            existing->setText(lossText);
            if (h.lossPercent > 5.0) {
                existing->setForeground(QColor(ui::kColorError));
            } else if (h.lossPercent > 0.0) {
                existing->setForeground(QColor(ui::kColorWarning));
            } else {
                existing->setForeground(QColor());
            }
        } else {
            auto* lossItem = new QTableWidgetItem(lossText);
            if (h.lossPercent > 5.0) {
                lossItem->setForeground(QColor(ui::kColorError));
            } else if (h.lossPercent > 0.0) {
                lossItem->setForeground(QColor(ui::kColorWarning));
            }
            m_mtrTable->setItem(i, 2, lossItem);
        }

        setOrCreate(i, 3, QString::number(h.sent));
        setOrCreate(i, 4, QStringLiteral("%1").arg(h.avgRttMs, 0, 'f', 1));
        setOrCreate(i, 5, QStringLiteral("%1").arg(h.bestRttMs, 0, 'f', 1));
        setOrCreate(i, 6, QStringLiteral("%1").arg(h.worstRttMs, 0, 'f', 1));
        setOrCreate(i, 7, QStringLiteral("%1").arg(h.jitterMs, 0, 'f', 2));
    }

    m_mtrTable->setSortingEnabled(true);
    m_mtrStatusLabel->setText(QStringLiteral("Cycle %1/%2").arg(cycle).arg(m_mtrCycles->value()));
}

void NetworkDiagnosticPanel::onMtrComplete(MtrResult result) {
    Q_ASSERT(m_mtrStartBtn);
    Q_ASSERT(m_mtrStopBtn);
    m_mtrStartBtn->setEnabled(true);
    m_mtrStopBtn->setEnabled(false);

    const int hops = static_cast<int>(result.hops.size());
    const QString status = QStringLiteral("MTR complete -- %1 hops, %2 cycles to %3")
                               .arg(hops)
                               .arg(result.totalCycles)
                               .arg(result.target);
    m_mtrStatusLabel->setText(status);
    Q_EMIT logOutput(status);
    Q_EMIT statusMessage(status, 5000);
}

// -- DNS --

void NetworkDiagnosticPanel::onDnsQuery() {
    Q_ASSERT(m_dnsHostname);
    Q_ASSERT(m_dnsQueryBtn);
    const auto hostname = m_dnsHostname->text().trimmed();
    if (hostname.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a hostname"), 3000);
        return;
    }

    m_dnsQueryBtn->setEnabled(false);
    m_dnsStatusLabel->setText(tr("Querying..."));
    // Use currentData if a known entry is selected; otherwise parse
    // the user-typed text for a manually-entered DNS server IP.
    const auto serverData = m_dnsServer->currentData();
    const auto server = serverData.isValid() ? serverData.toString()
                                             : m_dnsServer->currentText().trimmed();
    m_controller->dnsQuery(hostname, m_dnsRecordType->currentText(), server);
}

void NetworkDiagnosticPanel::onDnsReverseLookup() {
    Q_ASSERT(m_dnsHostname);
    Q_ASSERT(m_dnsReverseBtn);
    const auto ip = m_dnsHostname->text().trimmed();
    if (ip.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter an IP address"), 3000);
        return;
    }

    m_dnsReverseBtn->setEnabled(false);
    m_dnsStatusLabel->setText(tr("Resolving..."));
    const auto serverData = m_dnsServer->currentData();
    const auto server = serverData.isValid() ? serverData.toString()
                                             : m_dnsServer->currentText().trimmed();
    m_controller->dnsReverseLookup(ip, server);
}

void NetworkDiagnosticPanel::onDnsCompare() {
    Q_ASSERT(m_dnsHostname);
    Q_ASSERT(m_dnsServer);
    const auto hostname = m_dnsHostname->text().trimmed();
    if (hostname.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a hostname"), 3000);
        return;
    }

    QStringList servers;
    for (int i = 1; i < m_dnsServer->count(); ++i) {
        const auto addr = m_dnsServer->itemData(i).toString();
        if (!addr.isEmpty()) {
            servers.append(addr);
        }
    }

    m_dnsCompareBtn->setEnabled(false);
    m_dnsStatusLabel->setText(tr("Comparing DNS servers..."));
    m_controller->dnsCompare(hostname, m_dnsRecordType->currentText(), servers);
}

void NetworkDiagnosticPanel::onDnsFlushCache() {
    m_controller->dnsFlushCache();
}

void NetworkDiagnosticPanel::onDnsQueryComplete(DnsQueryResult result) {
    Q_ASSERT(m_dnsQueryBtn);
    Q_ASSERT(m_dnsReverseBtn);
    m_dnsQueryBtn->setEnabled(true);
    m_dnsReverseBtn->setEnabled(true);

    const int row = m_dnsTable->rowCount();
    m_dnsTable->insertRow(row);

    m_dnsTable->setItem(row, 0, new QTableWidgetItem(result.queryName));
    m_dnsTable->setItem(row, 1, new QTableWidgetItem(result.recordType));
    m_dnsTable->setItem(row, 2, new QTableWidgetItem(result.dnsServer));
    m_dnsTable->setItem(row,
                        3,
                        new QTableWidgetItem(
                            QStringLiteral("%1 ms").arg(result.responseTimeMs, 0, 'f', 1)));

    auto* answerItem = new QTableWidgetItem(result.answers.join(QStringLiteral(", ")));
    if (!result.success) {
        answerItem->setText(result.errorMessage);
        answerItem->setForeground(QColor(ui::kColorError));
    }
    m_dnsTable->setItem(row, 4, answerItem);

    if (result.success) {
        m_dnsStatusLabel->setText(QStringLiteral("%1 answers in %2 ms")
                                      .arg(result.answers.size())
                                      .arg(result.responseTimeMs, 0, 'f', 1));
    } else {
        m_dnsStatusLabel->setText(QStringLiteral("Query failed: %1").arg(result.errorMessage));
    }
}

void NetworkDiagnosticPanel::onDnsComparisonComplete(DnsServerComparison comparison) {
    Q_ASSERT(m_dnsCompareBtn);
    Q_ASSERT(m_dnsTable);
    m_dnsCompareBtn->setEnabled(true);

    m_dnsTable->setRowCount(0);
    for (const auto& result : comparison.results) {
        onDnsQueryComplete(result);
    }

    m_dnsStatusLabel->setText(
        comparison.allAgree
            ? tr("All servers agree")
            : tr("DNS servers returned different results -- possible DNS hijacking or caching"));
}

// -- Port Scanner --

namespace {

struct PortScanRange {
    uint16_t start = 0;
    uint16_t end = 0;
    bool hasPrimary = false;
};

QVector<uint16_t> getPresetPorts(int presetIdx) {
    if (presetIdx <= 0) {
        return {};
    }

    const auto presets = sak::PortScanner::getPresets();
    const int index = presetIdx - 1;
    if (index < 0 || index >= presets.size()) {
        return {};
    }
    return presets[index].ports;
}

void appendPortRange(QVector<uint16_t>& ports, uint16_t start, uint16_t end) {
    for (uint32_t p = start; p <= end; ++p) {
        ports.append(static_cast<uint16_t>(p));
    }
}

void handleParsedRange(QVector<uint16_t>& ports,
                       PortScanRange& range,
                       uint16_t start,
                       uint16_t end) {
    if (!range.hasPrimary) {
        range.start = start;
        range.end = end;
        range.hasPrimary = true;
        return;
    }
    appendPortRange(ports, start, end);
}

constexpr unsigned int kMaxPortValue = 65'535;

bool parsePortRange(const QString& text, QVector<uint16_t>& ports, PortScanRange& range) {
    const auto range_parts = text.split('-');
    if (range_parts.size() != 2) {
        return false;
    }

    bool ok_start = false;
    bool ok_end = false;
    const auto start_val = range_parts[0].trimmed().toUInt(&ok_start);
    const auto end_val = range_parts[1].trimmed().toUInt(&ok_end);

    if (!ok_start || !ok_end) {
        return false;
    }
    if (start_val == 0 || end_val == 0 || start_val > kMaxPortValue || end_val > kMaxPortValue ||
        start_val > end_val) {
        return false;
    }

    handleParsedRange(
        ports, range, static_cast<uint16_t>(start_val), static_cast<uint16_t>(end_val));
    return true;
}

bool parseSinglePort(const QString& text, QVector<uint16_t>& ports) {
    bool ok_port = false;
    const auto port_val = text.toUInt(&ok_port);
    if (!ok_port || port_val == 0 || port_val > kMaxPortValue) {
        return false;
    }
    ports.append(static_cast<uint16_t>(port_val));
    return true;
}

QVector<uint16_t> parseCustomPorts(const QString& customText, PortScanRange& range) {
    if (customText.isEmpty()) {
        return {21, 22, 23, 25, 53, 80, 110, 143, 443, 445, 993, 995, 3306, 3389, 5432, 8080, 8443};
    }

    QVector<uint16_t> ports;
    const auto parts = customText.split(',', Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        const auto trimmed = part.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (trimmed.contains('-')) {
            parsePortRange(trimmed, ports, range);
        } else {
            parseSinglePort(trimmed, ports);
        }
    }
    return ports;
}

}  // namespace

void NetworkDiagnosticPanel::onPortPresetChanged(int index) {
    Q_ASSERT(m_portCustomRange != nullptr);
    Q_ASSERT(m_portPreset != nullptr);

    if (index == 0) {
        m_portCustomRange->setEnabled(true);
        m_portCustomRange->clear();
        return;
    }

    const auto allPresets = PortScanner::getPresets();
    const int presetIndex = index - 1;
    if (presetIndex < 0 || presetIndex >= allPresets.size()) {
        m_portCustomRange->setEnabled(true);
        m_portCustomRange->clear();
        return;
    }

    const auto& preset = allPresets[presetIndex];
    QStringList portStrs;
    portStrs.reserve(preset.ports.size());
    for (auto port : preset.ports) {
        portStrs.append(QString::number(port));
    }

    m_portCustomRange->setText(portStrs.join(QStringLiteral(",")));
    m_portCustomRange->setEnabled(false);
}

void NetworkDiagnosticPanel::onStartPortScan() {
    Q_ASSERT(m_portTarget);
    Q_ASSERT(m_portTable);
    const auto target = m_portTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a port scan target"), 3000);
        return;
    }

    m_portTable->setRowCount(0);
    m_portSummaryLabel->clear();
    m_portProgress->setValue(0);
    m_portProgress->setVisible(true);
    m_portStartBtn->setEnabled(false);
    m_portStopBtn->setEnabled(true);

    PortScanRange range;
    QVector<uint16_t> ports;
    const int presetIdx = m_portPreset->currentIndex();
    if (presetIdx > 0) {
        ports = getPresetPorts(presetIdx);
    } else {
        ports = parseCustomPorts(m_portCustomRange->text().trimmed(), range);
    }

    m_controller->scanPorts({target,
                             ports,
                             range.start,
                             range.end,
                             m_portTimeout->value(),
                             m_portConcurrent->value(),
                             m_portBannerGrab->isChecked()});
}

void NetworkDiagnosticPanel::onStopPortScan() {
    Q_ASSERT(m_controller);
    m_controller->cancel();
    m_portStartBtn->setEnabled(true);
    m_portStopBtn->setEnabled(false);
    m_portProgress->setVisible(false);
}

void NetworkDiagnosticPanel::onPortScanned(PortScanResult result) {
    Q_ASSERT(m_portTable);
    // Only show open ports in real-time to avoid table bloat
    if (result.state != PortScanResult::State::Open) {
        return;
    }

    m_portTable->setSortingEnabled(false);
    const int row = m_portTable->rowCount();
    m_portTable->insertRow(row);

    m_portTable->setItem(row, 0, new QTableWidgetItem(QString::number(result.port)));

    auto* stateItem = new QTableWidgetItem(tr("Open"));
    stateItem->setForeground(QColor(ui::kColorSuccess));
    m_portTable->setItem(row, 1, stateItem);

    m_portTable->setItem(row, 2, new QTableWidgetItem(result.serviceName));
    m_portTable->setItem(
        row, 3, new QTableWidgetItem(QStringLiteral("%1").arg(result.responseTimeMs, 0, 'f', 1)));
    m_portTable->setItem(row, 4, new QTableWidgetItem(result.banner.left(200)));

    m_portTable->setSortingEnabled(true);
    m_portTable->scrollToBottom();
}

void NetworkDiagnosticPanel::onPortScanProgress(int scanned, int total) {
    if (total > 0) {
        m_portProgress->setValue(static_cast<int>(static_cast<double>(scanned) / total * 100.0));
    }
    m_portSummaryLabel->setText(QStringLiteral("%1/%2 scanned").arg(scanned).arg(total));
}

void NetworkDiagnosticPanel::onPortScanComplete(QVector<PortScanResult> results) {
    Q_ASSERT(m_portStartBtn);
    Q_ASSERT(m_portStopBtn);
    m_portStartBtn->setEnabled(true);
    m_portStopBtn->setEnabled(false);
    m_portProgress->setVisible(false);

    int openCount = 0;
    int closedCount = 0;
    int filteredCount = 0;
    for (const auto& r : results) {
        switch (r.state) {
        case PortScanResult::State::Open:
            ++openCount;
            break;
        case PortScanResult::State::Closed:
            ++closedCount;
            break;
        case PortScanResult::State::Filtered:
            ++filteredCount;
            break;
        default:
            break;
        }
    }

    m_portSummaryLabel->setText(QStringLiteral("Complete: %1 open, %2 closed, %3 filtered")
                                    .arg(openCount)
                                    .arg(closedCount)
                                    .arg(filteredCount));
}

// -- Bandwidth --

void NetworkDiagnosticPanel::onStartBandwidthTest() {
    Q_ASSERT(m_bwServerAddr);
    Q_ASSERT(m_bwResultLabel);
    const auto server = m_bwServerAddr->text().trimmed();
    if (server.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter an iPerf3 server address"), 3000);
        return;
    }

    m_bwResultLabel->setText(tr("Running bandwidth test..."));
    m_bwTestBtn->setEnabled(false);

    m_controller->runBandwidthTest({server,
                                    static_cast<uint16_t>(m_bwPort->value()),
                                    m_bwDuration->value(),
                                    m_bwStreams->value(),
                                    m_bwBidirectional->isChecked(),
                                    false});  // TCP mode
}

void NetworkDiagnosticPanel::onStartIperfServer() {
    m_controller->startIperfServer(static_cast<uint16_t>(m_bwPort->value()));
}

void NetworkDiagnosticPanel::onStopIperfServer() {
    m_controller->stopIperfServer();
}

void NetworkDiagnosticPanel::onRunHttpSpeedTest() {
    m_httpSpeedBtn->setEnabled(false);
    m_httpSpeedLabel->setText(tr("Running HTTP speed test..."));
    m_controller->runHttpSpeedTest();
}

void NetworkDiagnosticPanel::onBandwidthComplete(BandwidthTestResult result) {
    Q_ASSERT(m_bwTestBtn);
    Q_ASSERT(m_bwResultLabel);
    m_bwTestBtn->setEnabled(true);

    m_bwResultLabel->setText(QStringLiteral("<b>Download:</b> %1 Mbps | <b>Upload:</b> %2 Mbps<br>"
                                            "<b>Jitter:</b> %3 ms | <b>Packet Loss:</b> %4% | "
                                            "<b>Retransmits:</b> %5")
                                 .arg(result.downloadMbps, 0, 'f', 2)
                                 .arg(result.uploadMbps, 0, 'f', 2)
                                 .arg(result.jitterMs, 0, 'f', 2)
                                 .arg(result.packetLossPercent, 0, 'f', 2)
                                 .arg(result.retransmissions));
}

void NetworkDiagnosticPanel::onHttpSpeedComplete(double down, double up, double latency) {
    m_httpSpeedBtn->setEnabled(true);

    m_httpSpeedLabel->setText(QStringLiteral("<b>Download:</b> %1 Mbps | <b>Upload:</b> %2 Mbps | "
                                             "<b>Latency:</b> %3 ms")
                                  .arg(down, 0, 'f', 2)
                                  .arg(up, 0, 'f', 2)
                                  .arg(latency, 0, 'f', 1));
}

// -- WiFi --

void NetworkDiagnosticPanel::onScanWiFi() {
    if (!m_controller->isWiFiAvailable()) {
        Q_EMIT statusMessage(tr("WiFi hardware not available"), 3000);
        return;
    }
    m_wifiScanBtn->setEnabled(false);
    m_controller->scanWiFi();
}

void NetworkDiagnosticPanel::onStartContinuousWiFi() {
    if (!m_controller->isWiFiAvailable()) {
        Q_EMIT statusMessage(tr("WiFi hardware not available"), 3000);
        return;
    }
    m_wifiContBtn->setEnabled(false);
    m_wifiStopBtn->setEnabled(true);
    m_controller->startContinuousWiFiScan(5000);
}

void NetworkDiagnosticPanel::onStopContinuousWiFi() {
    m_controller->stopContinuousWiFiScan();
    m_wifiContBtn->setEnabled(true);
    m_wifiStopBtn->setEnabled(false);
}

void NetworkDiagnosticPanel::onWiFiScanComplete(QVector<WiFiNetworkInfo> networks) {
    Q_ASSERT(m_wifiScanBtn);
    Q_ASSERT(m_wifiTable);
    m_wifiScanBtn->setEnabled(true);
    m_wifiTable->setSortingEnabled(false);
    m_wifiTable->setRowCount(0);

    for (const auto& net : networks) {
        const int row = m_wifiTable->rowCount();
        m_wifiTable->insertRow(row);

        auto* ssidItem = new QTableWidgetItem(net.ssid);
        if (net.isConnected) {
            QFont f = ssidItem->font();
            f.setBold(true);
            ssidItem->setFont(f);
            ssidItem->setText(QStringLiteral("%1 *").arg(net.ssid));
        }
        m_wifiTable->setItem(row, 0, ssidItem);

        m_wifiTable->setItem(row, 1, new QTableWidgetItem(net.bssid));

        auto* signalItem = new QTableWidgetItem(QStringLiteral("%1 dBm").arg(net.rssiDbm));
        if (net.rssiDbm >= -50) {
            signalItem->setForeground(QColor(ui::kColorSuccess));
        } else if (net.rssiDbm >= -70) {
            signalItem->setForeground(QColor(ui::kColorWarning));
        } else {
            signalItem->setForeground(QColor(ui::kColorError));
        }
        m_wifiTable->setItem(row, 2, signalItem);

        m_wifiTable->setItem(row,
                             3,
                             new QTableWidgetItem(QStringLiteral("%1%").arg(net.signalQuality)));
        m_wifiTable->setItem(row, 4, new QTableWidgetItem(QString::number(net.channelNumber)));
        m_wifiTable->setItem(row, 5, new QTableWidgetItem(net.band));
        m_wifiTable->setItem(row, 6, new QTableWidgetItem(net.authentication));
        m_wifiTable->setItem(row, 7, new QTableWidgetItem(net.apVendor));
    }

    m_wifiTable->setSortingEnabled(true);
}

// -- Connections --

void NetworkDiagnosticPanel::onStartConnectionMonitor() {
    Q_ASSERT(m_connStartBtn);
    Q_ASSERT(m_connStopBtn);
    m_connStartBtn->setEnabled(false);
    m_connStopBtn->setEnabled(true);

    m_controller->startConnectionMonitor(m_connRefreshRate->value(),
                                         m_connShowTcp->isChecked(),
                                         m_connShowUdp->isChecked(),
                                         m_connProcessFilter->text().trimmed(),
                                         0);
}

void NetworkDiagnosticPanel::onStopConnectionMonitor() {
    m_controller->stopConnectionMonitor();
    m_connStartBtn->setEnabled(true);
    m_connStopBtn->setEnabled(false);
}

void NetworkDiagnosticPanel::onConnectionsUpdated(QVector<ConnectionInfo> connections) {
    Q_ASSERT(m_connTable);
    Q_ASSERT(m_connSummaryLabel);
    m_connTable->setSortingEnabled(false);
    m_connTable->setRowCount(connections.size());

    int tcpCount = 0;
    int udpCount = 0;
    int established = 0;

    auto setOrCreate = [this](int row, int col, const QString& text) {
        if (auto* existing = m_connTable->item(row, col)) {
            existing->setText(text);
        } else {
            m_connTable->setItem(row, col, new QTableWidgetItem(text));
        }
    };

    auto stateColor = [](const QString& state) -> QColor {
        if (state == QStringLiteral("ESTABLISHED")) {
            return QColor(ui::kColorSuccess);
        }
        if (state == QStringLiteral("CLOSE_WAIT") || state == QStringLiteral("TIME_WAIT")) {
            return QColor(ui::kColorWarning);
        }
        return {};
    };

    for (int i = 0; i < connections.size(); ++i) {
        const auto& conn = connections[i];
        const bool is_tcp = (conn.protocol == ConnectionInfo::Protocol::TCP);
        is_tcp ? ++tcpCount : ++udpCount;
        if (conn.state == QStringLiteral("ESTABLISHED")) {
            ++established;
        }

        setOrCreate(i, 0, is_tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"));
        setOrCreate(i, 1, conn.localAddress);
        setOrCreate(i, 2, QString::number(conn.localPort));
        setOrCreate(i, 3, conn.remoteAddress);
        setOrCreate(i, 4, QString::number(conn.remotePort));

        setOrCreate(i, 5, conn.state);
        m_connTable->item(i, 5)->setForeground(stateColor(conn.state));

        setOrCreate(i, 6, conn.processName);
    }

    m_connTable->setSortingEnabled(true);
    m_connSummaryLabel->setText(QStringLiteral("Total: %1 | TCP: %2 | UDP: %3 | Established: %4")
                                    .arg(connections.size())
                                    .arg(tcpCount)
                                    .arg(udpCount)
                                    .arg(established));
}

// -- Firewall --

void NetworkDiagnosticPanel::onAuditFirewall() {
    m_fwAuditBtn->setEnabled(false);
    m_fwRuleTable->setRowCount(0);
    m_fwConflictText->clear();
    m_fwGapText->clear();
    m_fwSummaryLabel->setText(tr("Auditing..."));
    m_controller->auditFirewall();
}

void NetworkDiagnosticPanel::onFirewallAuditComplete(QVector<FirewallRule> rules,
                                                     QVector<FirewallConflict> conflicts,
                                                     QVector<FirewallGap> gaps) {
    m_cachedFwRules = rules;
    filterFirewallRules();
    m_fwAuditBtn->setEnabled(true);

    // Populate conflicts
    if (conflicts.isEmpty()) {
        m_fwConflictText->setPlainText(tr("No conflicts detected."));
    } else {
        QString text;
        for (const auto& c : conflicts) {
            const auto severity = (c.severity == FirewallConflict::Severity::Critical)
                                      ? QStringLiteral("[CRITICAL]")
                                  : (c.severity == FirewallConflict::Severity::Warning)
                                      ? QStringLiteral("[WARNING]")
                                      : QStringLiteral("[INFO]");
            text += QStringLiteral("%1 %2\n\n").arg(severity, c.conflictDescription);
        }
        m_fwConflictText->setPlainText(text);
    }

    // Populate gaps
    if (gaps.isEmpty()) {
        m_fwGapText->setPlainText(tr("No coverage gaps detected."));
    } else {
        QString text;
        for (const auto& g : gaps) {
            text += QStringLiteral("* %1\n  Recommendation: %2\n\n")
                        .arg(g.description, g.recommendation);
        }
        m_fwGapText->setPlainText(text);
    }

    m_fwSummaryLabel->setText(QStringLiteral("%1 rules | %2 conflicts | %3 gaps")
                                  .arg(rules.size())
                                  .arg(conflicts.size())
                                  .arg(gaps.size()));
}

namespace {

bool matchesDirectionFilter(FirewallRule::Direction direction, int dir_idx) {
    if (dir_idx == 1 && direction != FirewallRule::Direction::Inbound) {
        return false;
    }
    if (dir_idx == 2 && direction != FirewallRule::Direction::Outbound) {
        return false;
    }
    return true;
}

bool matchesActionFilter(FirewallRule::Action action, int act_idx) {
    if (act_idx == 1 && action != FirewallRule::Action::Allow) {
        return false;
    }
    if (act_idx == 2 && action != FirewallRule::Action::Block) {
        return false;
    }
    return true;
}

bool matchesSearchText(const FirewallRule& rule, const QString& text) {
    return rule.name.contains(text, Qt::CaseInsensitive) ||
           rule.applicationPath.contains(text, Qt::CaseInsensitive) ||
           rule.localPorts.contains(text, Qt::CaseInsensitive) ||
           rule.remotePorts.contains(text, Qt::CaseInsensitive);
}

QString protocolToString(FirewallRule::Protocol protocol) {
    switch (protocol) {
    case FirewallRule::Protocol::TCP:
        return QStringLiteral("TCP");
    case FirewallRule::Protocol::UDP:
        return QStringLiteral("UDP");
    case FirewallRule::Protocol::ICMPv4:
        return QStringLiteral("ICMPv4");
    case FirewallRule::Protocol::ICMPv6:
        return QStringLiteral("ICMPv6");
    case FirewallRule::Protocol::Any:
        return QStringLiteral("Any");
    }
    return QStringLiteral("Unknown");
}

}  // namespace

void NetworkDiagnosticPanel::filterFirewallRules() {
    Q_ASSERT(m_fwSearchBox);
    Q_ASSERT(m_fwDirFilter);
    const auto search_text = m_fwSearchBox->text().trimmed();
    const int dir_idx = m_fwDirFilter->currentIndex();
    const int act_idx = m_fwActionFilter->currentIndex();

    auto matches = [&](const FirewallRule& r) {
        if (!matchesDirectionFilter(r.direction, dir_idx)) {
            return false;
        }
        if (!matchesActionFilter(r.action, act_idx)) {
            return false;
        }
        if (search_text.isEmpty()) {
            return true;
        }
        return matchesSearchText(r, search_text);
    };

    QVector<FirewallRule> filtered;
    filtered.reserve(m_cachedFwRules.size());
    std::copy_if(
        m_cachedFwRules.begin(), m_cachedFwRules.end(), std::back_inserter(filtered), matches);

    populateFirewallTable(filtered);
}

void NetworkDiagnosticPanel::populateFirewallTable(const QVector<FirewallRule>& filtered) {
    Q_ASSERT(m_fwRuleTable);

    m_fwRuleTable->setSortingEnabled(false);
    m_fwRuleTable->setRowCount(filtered.size());

    for (int i = 0; i < filtered.size(); ++i) {
        const auto& r = filtered[i];
        auto* enabledItem = new QTableWidgetItem(r.enabled ? tr("Yes") : tr("No"));
        if (!r.enabled) {
            enabledItem->setForeground(QColor(ui::kColorTextMuted));
        }
        m_fwRuleTable->setItem(i, 0, enabledItem);
        m_fwRuleTable->setItem(i, 1, new QTableWidgetItem(r.name));

        const auto dirStr = (r.direction == FirewallRule::Direction::Inbound) ? tr("Inbound")
                                                                              : tr("Outbound");
        m_fwRuleTable->setItem(i, 2, new QTableWidgetItem(dirStr));

        auto* actionItem = new QTableWidgetItem(
            (r.action == FirewallRule::Action::Allow) ? tr("Allow") : tr("Block"));
        actionItem->setForeground((r.action == FirewallRule::Action::Allow)
                                      ? QColor(ui::kColorSuccess)
                                      : QColor(ui::kColorError));
        m_fwRuleTable->setItem(i, 3, actionItem);

        m_fwRuleTable->setItem(i, 4, new QTableWidgetItem(protocolToString(r.protocol)));
        m_fwRuleTable->setItem(i, 5, new QTableWidgetItem(r.localPorts));
        m_fwRuleTable->setItem(i, 6, new QTableWidgetItem(r.remotePorts));
        m_fwRuleTable->setItem(i, 7, new QTableWidgetItem(r.applicationPath));
    }
    m_fwRuleTable->setSortingEnabled(true);
}

// -- Shares --

void NetworkDiagnosticPanel::onDiscoverShares() {
    m_shareDiscoverBtn->setEnabled(false);
    auto hostname = m_shareHostname->text().trimmed();
    if (hostname.isEmpty()) {
        hostname = QStringLiteral("localhost");
    }
    m_shareTable->setRowCount(0);
    m_controller->discoverShares(hostname);
}

void NetworkDiagnosticPanel::onSharesDiscovered(QVector<NetworkShareInfo> shares) {
    Q_ASSERT(m_shareDiscoverBtn);
    Q_ASSERT(m_shareTable);
    m_shareDiscoverBtn->setEnabled(true);

    m_shareTable->setRowCount(shares.size());

    for (int i = 0; i < shares.size(); ++i) {
        const auto& s = shares[i];
        m_shareTable->setItem(i, 0, new QTableWidgetItem(s.shareName));

        auto typeStr = QStringLiteral("Disk");
        switch (s.type) {
        case NetworkShareInfo::ShareType::Printer:
            typeStr = QStringLiteral("Printer");
            break;
        case NetworkShareInfo::ShareType::Device:
            typeStr = QStringLiteral("Device");
            break;
        case NetworkShareInfo::ShareType::IPC:
            typeStr = QStringLiteral("IPC");
            break;
        case NetworkShareInfo::ShareType::Special:
            typeStr = QStringLiteral("Special");
            break;
        default:
            break;
        }
        m_shareTable->setItem(i, 1, new QTableWidgetItem(typeStr));

        auto* readItem = new QTableWidgetItem(s.canRead ? tr("Yes") : tr("No"));
        readItem->setForeground(s.canRead ? QColor(ui::kColorSuccess) : QColor(ui::kColorError));
        m_shareTable->setItem(i, 2, readItem);

        auto* writeItem = new QTableWidgetItem(s.canWrite ? tr("Yes") : tr("No"));
        writeItem->setForeground(s.canWrite ? QColor(ui::kColorSuccess) : QColor(ui::kColorError));
        m_shareTable->setItem(i, 3, writeItem);

        m_shareTable->setItem(i, 4, new QTableWidgetItem(s.remark));
    }
}

// -- LAN Transfer Slots --

void NetworkDiagnosticPanel::onStartLanTransferServer() {
    m_lanServerStartBtn->setEnabled(false);
    m_controller->startLanTransferServer(static_cast<uint16_t>(m_lanPort->value()));
}

void NetworkDiagnosticPanel::onStopLanTransferServer() {
    m_controller->stopLanTransferServer();
}

void NetworkDiagnosticPanel::onRunLanTransferTest() {
    Q_ASSERT(m_lanTarget);
    Q_ASSERT(m_lanTestBtn);
    const auto target = m_lanTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Enter the target device IP address"), 3000);
        m_lanTarget->setFocus();
        return;
    }
    m_lanTestBtn->setEnabled(false);
    m_lanResultLabel->setText(tr("Running transfer test..."));
    m_controller->runLanTransferTest(target,
                                     static_cast<uint16_t>(m_lanPort->value()),
                                     m_lanDuration->value(),
                                     m_lanBlockSize->value());
}

void NetworkDiagnosticPanel::onLanTransferProgress(double currentMbps,
                                                   double elapsedSec,
                                                   qint64 totalBytes) {
    m_lanResultLabel->setText(QStringLiteral("Running: %1 Mbps | %2 s | %3 MB transferred")
                                  .arg(currentMbps, 0, 'f', 1)
                                  .arg(elapsedSec, 0, 'f', 0)
                                  .arg(totalBytes / (1024.0 * 1024.0), 0, 'f', 1));
}

void NetworkDiagnosticPanel::onLanTransferComplete(LanTransferResult result) {
    Q_ASSERT(m_lanTestBtn);
    Q_ASSERT(m_lanResultLabel);
    m_lanTestBtn->setEnabled(true);
    m_lanResultLabel->setText(QStringLiteral("<b>%1 Complete</b><br>"
                                             "Remote: %2<br>"
                                             "Transferred: %3 MB in %4 s<br>"
                                             "Average Speed: <b>%5 Mbps</b> (%6 MB/s)<br>"
                                             "Peak Speed: %7 Mbps")
                                  .arg(result.isUpload ? tr("Upload") : tr("Download"))
                                  .arg(result.remoteAddress)
                                  .arg(result.bytesTransferred / (1024.0 * 1024.0), 0, 'f', 1)
                                  .arg(result.durationSec, 0, 'f', 1)
                                  .arg(result.avgSpeedMbps, 0, 'f', 1)
                                  .arg(result.avgSpeedMbps / 8.0, 0, 'f', 1)
                                  .arg(result.peakSpeedMbps, 0, 'f', 1));
}

// -- Controller State --

void NetworkDiagnosticPanel::onStateChanged(int newState) {
    Q_ASSERT(m_refreshBtn);
    Q_ASSERT(m_pingStartBtn);
    const auto state = static_cast<NetworkDiagnosticController::State>(newState);
    const bool idle = (state == NetworkDiagnosticController::State::Idle);

    // Re-enable buttons when idle
    if (idle) {
        m_refreshBtn->setEnabled(true);
        m_pingStartBtn->setEnabled(true);
        m_pingStopBtn->setEnabled(false);
        m_traceStartBtn->setEnabled(true);
        m_traceStopBtn->setEnabled(false);
        m_mtrStartBtn->setEnabled(true);
        m_mtrStopBtn->setEnabled(false);
        m_portStartBtn->setEnabled(true);
        m_portStopBtn->setEnabled(false);
        m_bwTestBtn->setEnabled(true);
        m_wifiScanBtn->setEnabled(true);
        m_wifiContBtn->setEnabled(true);
        m_wifiStopBtn->setEnabled(false);
        m_connStartBtn->setEnabled(true);
        m_connStopBtn->setEnabled(false);
        m_dnsQueryBtn->setEnabled(true);
        m_dnsReverseBtn->setEnabled(true);
        m_dnsCompareBtn->setEnabled(true);
        m_dnsFlushBtn->setEnabled(true);
        m_httpSpeedBtn->setEnabled(true);
        m_fwAuditBtn->setEnabled(true);
        m_shareDiscoverBtn->setEnabled(true);
        m_lanTestBtn->setEnabled(true);
    }
}

void NetworkDiagnosticPanel::onError(QString error) {
    Q_ASSERT(m_bwResultLabel);
    Q_ASSERT(m_httpSpeedLabel);
    Q_EMIT logOutput(QStringLiteral("[ERROR] %1").arg(error));
    Q_EMIT statusMessage(error, 5000);

    // Defensively re-enable all controls in case the controller doesn't
    // transition back to Idle after an error
    onStateChanged(static_cast<int>(NetworkDiagnosticController::State::Idle));

    // Clear any "Running..." labels stuck by incomplete operations
    m_bwResultLabel->clear();
    m_httpSpeedLabel->clear();
}

// ===================================================================
// Quick Actions — Reset Network
// ===================================================================

void NetworkDiagnosticPanel::createResetNetworkAction() {
    m_qa_controller = new QuickActionController(this);

    m_qa_controller->registerAction(std::make_unique<ResetNetworkAction>());

    connect(m_qa_controller,
            &QuickActionController::actionExecutionComplete,
            this,
            &NetworkDiagnosticPanel::onResetNetworkComplete,
            Qt::QueuedConnection);
    connect(m_qa_controller,
            &QuickActionController::actionError,
            this,
            &NetworkDiagnosticPanel::onResetNetworkError,
            Qt::QueuedConnection);
    connect(
        m_qa_controller,
        &QuickActionController::logMessage,
        this,
        [this](const QString& msg) { Q_EMIT logOutput(msg); },
        Qt::QueuedConnection);

    connect(m_resetNetworkBtn,
            &QPushButton::clicked,
            this,
            &NetworkDiagnosticPanel::onResetNetworkClicked);
}

void NetworkDiagnosticPanel::onResetNetworkClicked() {
    Q_EMIT logOutput(QStringLiteral("Executing: Reset Network Settings"));
    m_resetNetworkBtn->setEnabled(false);
    m_resetNetworkBtn->setText(tr("Resetting..."));
    m_qa_controller->executeAction(QStringLiteral("Reset Network Settings"), false);
}

void NetworkDiagnosticPanel::onResetNetworkComplete(QuickAction* action) {
    Q_ASSERT(action);
    m_resetNetworkBtn->setEnabled(true);
    m_resetNetworkBtn->setText(tr("Reset Network"));
    const auto& result = action->lastExecutionResult();
    const QString msg = result.success ? tr("Network settings reset successfully")
                                       : QString("Reset failed: %1").arg(result.message);
    Q_EMIT logOutput(msg);
    Q_EMIT statusMessage(msg, sak::kTimerStatusDefaultMs);
}

void NetworkDiagnosticPanel::onResetNetworkError(QuickAction* action, const QString& error) {
    Q_ASSERT(action);
    m_resetNetworkBtn->setEnabled(true);
    m_resetNetworkBtn->setText(tr("Reset Network"));
    Q_EMIT logOutput(QString("Error: Reset Network - %1").arg(error));
    Q_EMIT statusMessage(QString("Reset Network failed: %1").arg(error),
                         sak::kTimerStatusDefaultMs);
    sak::logError("Reset network error: {}", error.toStdString());
}

// ===================================================================
// Result Table Context Menus
// ===================================================================

void NetworkDiagnosticPanel::copySelectedRows(QTableWidget* table) {
    Q_ASSERT(table);
    const auto rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }

    QString text;
    for (const auto& idx : rows) {
        QStringList cells;
        for (int col = 0; col < table->columnCount(); ++col) {
            auto* item = table->item(idx.row(), col);
            cells << (item ? item->text() : QString());
        }
        text += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
    }
    QApplication::clipboard()->setText(text.trimmed());
    Q_EMIT statusMessage(tr("Copied %1 row(s) to clipboard").arg(rows.size()), 2000);
}

void NetworkDiagnosticPanel::copyAllRows(QTableWidget* table) {
    Q_ASSERT(table);
    if (table->rowCount() == 0) {
        return;
    }

    QString text;
    // Header row
    QStringList headers;
    for (int col = 0; col < table->columnCount(); ++col) {
        auto* item = table->horizontalHeaderItem(col);
        headers << (item ? item->text() : QString());
    }
    text += headers.join(QLatin1Char('\t')) + QLatin1Char('\n');

    // Data rows
    for (int row = 0; row < table->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < table->columnCount(); ++col) {
            auto* item = table->item(row, col);
            cells << (item ? item->text() : QString());
        }
        text += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
    }
    QApplication::clipboard()->setText(text.trimmed());
    Q_EMIT statusMessage(tr("Copied %1 row(s) to clipboard").arg(table->rowCount()), 2000);
}

void NetworkDiagnosticPanel::exportTableToCsv(QTableWidget* table, const QString& default_name) {
    Q_ASSERT(table);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export to CSV"), default_name, tr("CSV Files (*.csv)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT statusMessage(tr("Failed to open file for writing"), 3000);
        return;
    }

    QTextStream out(&file);
    // Header
    QStringList headers;
    for (int col = 0; col < table->columnCount(); ++col) {
        auto* item = table->horizontalHeaderItem(col);
        headers << QStringLiteral("\"%1\"").arg(item ? item->text() : QString());
    }
    out << headers.join(QLatin1Char(',')) << "\n";

    // Data
    for (int row = 0; row < table->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < table->columnCount(); ++col) {
            auto* item = table->item(row, col);
            QString cell_text = item ? item->text() : QString();
            cell_text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            cells << QStringLiteral("\"%1\"").arg(cell_text);
        }
        out << cells.join(QLatin1Char(',')) << "\n";
    }

    Q_EMIT statusMessage(tr("Exported %1 rows to %2").arg(table->rowCount()).arg(path), 3000);
    Q_EMIT logOutput(QStringLiteral("Exported results to %1").arg(path));
}

void NetworkDiagnosticPanel::copyTableCellValue(QTableWidget* table, int column) {
    Q_ASSERT(table);
    const int row = table->currentRow();
    if (row < 0) {
        return;
    }
    auto* item = table->item(row, column);
    if (item && !item->text().isEmpty()) {
        QApplication::clipboard()->setText(item->text());
        Q_EMIT statusMessage(tr("Copied: %1").arg(item->text()), 2000);
    }
}

void NetworkDiagnosticPanel::addCommonTableActions(QMenu& menu,
                                                   QTableWidget* table,
                                                   const QString& export_name) {
    menu.addSeparator();
    menu.addAction(tr("Copy Selected Row(s)"), this, [this, table]() { copySelectedRows(table); });
    menu.addAction(tr("Copy All Results"), this, [this, table]() { copyAllRows(table); });
    menu.addAction(tr("Export to CSV..."), this, [this, table, export_name]() {
        exportTableToCsv(table, export_name);
    });
    menu.addSeparator();
    menu.addAction(tr("Clear Results"), this, [table]() { table->setRowCount(0); });
}

void NetworkDiagnosticPanel::showPingContextMenu(const QPoint& pos) {
    if (m_pingTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_pingTable->currentRow() >= 0) {
        menu.addAction(tr("Copy IP Address"), this, [this]() {
            copyTableCellValue(m_pingTable, 1);
        });
        menu.addAction(tr("Copy RTT"), this, [this]() { copyTableCellValue(m_pingTable, 3); });
        menu.addSeparator();
        menu.addAction(tr("Traceroute to Target"), this, [this]() {
            const QString target = m_pingTarget->text().trimmed();
            if (!target.isEmpty()) {
                m_traceTarget->setText(target);
                m_toolTabs->setCurrentIndex(1);
                onStartTraceroute();
            }
        });
    }

    addCommonTableActions(menu, m_pingTable, QStringLiteral("ping_results.csv"));
    menu.exec(m_pingTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::showTracerouteContextMenu(const QPoint& pos) {
    if (m_traceTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_traceTable->currentRow() >= 0) {
        menu.addAction(tr("Copy IP Address"), this, [this]() {
            copyTableCellValue(m_traceTable, 1);
        });
        menu.addAction(tr("Copy Hostname"), this, [this]() {
            copyTableCellValue(m_traceTable, 2);
        });
        menu.addSeparator();

        auto* ip_item = m_traceTable->item(m_traceTable->currentRow(), 1);
        if (ip_item && !ip_item->text().isEmpty() && ip_item->text() != QStringLiteral("*")) {
            menu.addAction(tr("Ping this Hop"), this, [this, ip_item]() {
                m_pingTarget->setText(ip_item->text());
                m_toolTabs->setCurrentIndex(0);
                onStartPing();
            });
        }
    }

    addCommonTableActions(menu, m_traceTable, QStringLiteral("traceroute_results.csv"));
    menu.exec(m_traceTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::showMtrContextMenu(const QPoint& pos) {
    if (m_mtrTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_mtrTable->currentRow() >= 0) {
        menu.addAction(tr("Copy IP/Hostname"), this, [this]() {
            copyTableCellValue(m_mtrTable, 1);
        });
        menu.addAction(tr("Copy Loss %"), this, [this]() { copyTableCellValue(m_mtrTable, 2); });
        menu.addSeparator();

        auto* ip_item = m_mtrTable->item(m_mtrTable->currentRow(), 1);
        if (ip_item && !ip_item->text().isEmpty() && ip_item->text() != QStringLiteral("*")) {
            menu.addAction(tr("Ping this Hop"), this, [this, ip_item]() {
                m_pingTarget->setText(ip_item->text());
                m_toolTabs->setCurrentIndex(0);
                onStartPing();
            });
            menu.addAction(tr("Traceroute to Hop"), this, [this, ip_item]() {
                m_traceTarget->setText(ip_item->text());
                m_toolTabs->setCurrentIndex(1);
                onStartTraceroute();
            });
        }
    }

    addCommonTableActions(menu, m_mtrTable, QStringLiteral("mtr_results.csv"));
    menu.exec(m_mtrTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::showDnsContextMenu(const QPoint& pos) {
    if (m_dnsTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_dnsTable->currentRow() >= 0) {
        menu.addAction(tr("Copy Query"), this, [this]() { copyTableCellValue(m_dnsTable, 0); });
        menu.addAction(tr("Copy Answers"), this, [this]() { copyTableCellValue(m_dnsTable, 4); });
        menu.addAction(tr("Copy Server"), this, [this]() { copyTableCellValue(m_dnsTable, 2); });
        menu.addSeparator();

        auto* answer_item = m_dnsTable->item(m_dnsTable->currentRow(), 4);
        if (answer_item && !answer_item->text().isEmpty()) {
            menu.addAction(tr("Ping First Answer"), this, [this, answer_item]() {
                const QString first_answer =
                    answer_item->text().split(QLatin1Char(',')).first().trimmed();
                if (!first_answer.isEmpty()) {
                    m_pingTarget->setText(first_answer);
                    m_toolTabs->setCurrentIndex(0);
                    onStartPing();
                }
            });
        }
    }

    addCommonTableActions(menu, m_dnsTable, QStringLiteral("dns_results.csv"));
    menu.exec(m_dnsTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::showPortScanContextMenu(const QPoint& pos) {
    if (m_portTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_portTable->currentRow() >= 0) {
        menu.addAction(tr("Copy Port"), this, [this]() { copyTableCellValue(m_portTable, 0); });
        menu.addAction(tr("Copy Service"), this, [this]() { copyTableCellValue(m_portTable, 2); });
        menu.addAction(tr("Copy Banner"), this, [this]() { copyTableCellValue(m_portTable, 4); });
        menu.addSeparator();
        menu.addAction(tr("Copy Port:Service"), this, [this]() {
            int row = m_portTable->currentRow();
            auto* port_item = m_portTable->item(row, 0);
            auto* svc_item = m_portTable->item(row, 2);
            if (port_item) {
                QString text = port_item->text();
                if (svc_item && !svc_item->text().isEmpty()) {
                    text += QStringLiteral(" (%1)").arg(svc_item->text());
                }
                QApplication::clipboard()->setText(text);
                Q_EMIT statusMessage(tr("Copied: %1").arg(text), 2000);
            }
        });
    }

    addCommonTableActions(menu, m_portTable, QStringLiteral("port_scan_results.csv"));
    menu.exec(m_portTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::showWiFiContextMenu(const QPoint& pos) {
    if (m_wifiTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_wifiTable->currentRow() >= 0) {
        menu.addAction(tr("Copy SSID"), this, [this]() { copyTableCellValue(m_wifiTable, 0); });
        menu.addAction(tr("Copy BSSID"), this, [this]() { copyTableCellValue(m_wifiTable, 1); });
        menu.addAction(tr("Copy Signal/Quality"), this, [this]() {
            int row = m_wifiTable->currentRow();
            auto* sig = m_wifiTable->item(row, 2);
            auto* qual = m_wifiTable->item(row, 3);
            QString text;
            if (sig) {
                text = sig->text();
            }
            if (qual) {
                text += QStringLiteral(" (%1)").arg(qual->text());
            }
            QApplication::clipboard()->setText(text);
        });
        menu.addSeparator();
        menu.addAction(tr("Copy Channel/Band"), this, [this]() {
            int row = m_wifiTable->currentRow();
            auto* ch = m_wifiTable->item(row, 4);
            auto* band = m_wifiTable->item(row, 5);
            QString text;
            if (ch) {
                text = QStringLiteral("Ch %1").arg(ch->text());
            }
            if (band) {
                text += QStringLiteral(" (%1)").arg(band->text());
            }
            QApplication::clipboard()->setText(text);
        });
    }

    addCommonTableActions(menu, m_wifiTable, QStringLiteral("wifi_scan_results.csv"));
    menu.exec(m_wifiTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::showConnectionsContextMenu(const QPoint& pos) {
    if (m_connTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_connTable->currentRow() >= 0) {
        menu.addAction(tr("Copy Remote Address"), this, [this]() {
            copyTableCellValue(m_connTable, 3);
        });
        menu.addAction(tr("Copy Remote Address:Port"), this, [this]() {
            int row = m_connTable->currentRow();
            auto* addr = m_connTable->item(row, 3);
            auto* port = m_connTable->item(row, 4);
            if (addr) {
                QString text = addr->text();
                if (port && !port->text().isEmpty()) {
                    text += QLatin1Char(':') + port->text();
                }
                QApplication::clipboard()->setText(text);
                Q_EMIT statusMessage(tr("Copied: %1").arg(text), 2000);
            }
        });
        menu.addAction(tr("Copy Process"), this, [this]() { copyTableCellValue(m_connTable, 6); });
        menu.addSeparator();

        auto* remote_item = m_connTable->item(m_connTable->currentRow(), 3);
        if (remote_item && !remote_item->text().isEmpty() &&
            remote_item->text() != QStringLiteral("0.0.0.0") &&
            remote_item->text() != QStringLiteral("::")) {
            menu.addAction(tr("Ping Remote Address"), this, [this, remote_item]() {
                m_pingTarget->setText(remote_item->text());
                m_toolTabs->setCurrentIndex(0);
                onStartPing();
            });
            menu.addAction(tr("Traceroute to Remote"), this, [this, remote_item]() {
                m_traceTarget->setText(remote_item->text());
                m_toolTabs->setCurrentIndex(1);
                onStartTraceroute();
            });
            menu.addAction(tr("DNS Reverse Lookup"), this, [this, remote_item]() {
                m_dnsHostname->setText(remote_item->text());
                m_toolTabs->setCurrentIndex(3);
                onDnsReverseLookup();
            });
        }
    }

    addCommonTableActions(menu, m_connTable, QStringLiteral("connections_results.csv"));
    menu.exec(m_connTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::copyFirewallPorts() {
    int row = m_fwRuleTable->currentRow();
    auto* local = m_fwRuleTable->item(row, 5);
    auto* remote = m_fwRuleTable->item(row, 6);
    QStringList parts;
    if (local && !local->text().isEmpty()) {
        parts << QStringLiteral("Local: %1").arg(local->text());
    }
    if (remote && !remote->text().isEmpty()) {
        parts << QStringLiteral("Remote: %1").arg(remote->text());
    }
    if (!parts.isEmpty()) {
        QApplication::clipboard()->setText(parts.join(QStringLiteral(", ")));
    }
}

void NetworkDiagnosticPanel::copyFirewallRuleDetails() {
    int row = m_fwRuleTable->currentRow();
    QStringList details;
    for (int col = 0; col < m_fwRuleTable->columnCount(); ++col) {
        auto* header = m_fwRuleTable->horizontalHeaderItem(col);
        auto* item = m_fwRuleTable->item(row, col);
        if (header && item) {
            details << QStringLiteral("%1: %2").arg(header->text(), item->text());
        }
    }
    QApplication::clipboard()->setText(details.join(QLatin1Char('\n')));
    Q_EMIT statusMessage(tr("Copied full rule details"), 2000);
}

void NetworkDiagnosticPanel::showFirewallContextMenu(const QPoint& pos) {
    if (m_fwRuleTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_fwRuleTable->currentRow() >= 0) {
        menu.addAction(tr("Copy Rule Name"), this, [this]() {
            copyTableCellValue(m_fwRuleTable, 1);
        });
        menu.addAction(tr("Copy Application"), this, [this]() {
            copyTableCellValue(m_fwRuleTable, 7);
        });
        menu.addAction(tr("Copy Ports"), this, [this]() { copyFirewallPorts(); });
        menu.addSeparator();
        menu.addAction(tr("Copy Full Rule Details"), this, [this]() { copyFirewallRuleDetails(); });
    }

    addCommonTableActions(menu, m_fwRuleTable, QStringLiteral("firewall_rules.csv"));
    menu.exec(m_fwRuleTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::showSharesContextMenu(const QPoint& pos) {
    if (m_shareTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_shareTable->currentRow() >= 0) {
        menu.addAction(tr("Copy Share Name"), this, [this]() {
            copyTableCellValue(m_shareTable, 0);
        });

        auto* share_item = m_shareTable->item(m_shareTable->currentRow(), 0);
        const QString host = m_shareHostname->text().trimmed();
        if (share_item && !host.isEmpty()) {
            const QString unc_path = QStringLiteral("\\\\%1\\%2").arg(host, share_item->text());
            menu.addAction(tr("Copy UNC Path"), this, [this, unc_path]() {
                QApplication::clipboard()->setText(unc_path);
                Q_EMIT statusMessage(tr("Copied: %1").arg(unc_path), 2000);
            });
            menu.addAction(tr("Open in Explorer"), this, [unc_path]() {
                QProcess::startDetached(QStringLiteral("explorer.exe"), {unc_path});
            });
        }

        menu.addAction(tr("Copy Remark"), this, [this]() { copyTableCellValue(m_shareTable, 4); });
    }

    addCommonTableActions(menu, m_shareTable, QStringLiteral("network_shares.csv"));
    menu.exec(m_shareTable->viewport()->mapToGlobal(pos));
}

}  // namespace sak
