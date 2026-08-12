// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_panel.cpp
/// @brief Main UI panel for Network Diagnostics & Troubleshooting

#include "sak/network_diagnostic_panel.h"

#include "sak/action_constants.h"
#include "sak/actions/reset_network_action.h"
#include "sak/detachable_log_window.h"
#include "sak/dns_diagnostic_tool.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/network_constants.h"
#include "sak/network_diagnostic_controller.h"
#include "sak/port_scanner.h"
#include "sak/process_runner.h"
#include "sak/quick_action_controller.h"
#include "sak/style_constants.h"
#include "sak/view_empty_state.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFutureWatcher>
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
#include <QtConcurrent>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace sak {

namespace {

constexpr int kNetworkDetailMinHeight = 50;
constexpr int kFirewallAnalysisMaxHeight = 120;
constexpr int kAdapterSelectDialogMinWidth = 350;
constexpr int kTableColumnWidthTiny = 50;
constexpr int kTableColumnWidthSmall = 60;
constexpr int kTableColumnWidthCompact = 65;
constexpr int kTableColumnWidthPort = 70;
constexpr int kTableColumnWidthMedium = 75;
constexpr int kTableColumnWidthStandard = 80;
constexpr int kTableColumnWidthNarrow = 90;
constexpr int kTableColumnWidthRemotePort = 95;
constexpr int kTableColumnWidthWide = 100;
constexpr int kTableColumnWidthState = 105;
constexpr int kTableColumnWidthRadio = 110;
constexpr int kTableColumnWidthAddress = 120;
constexpr int kTableColumnWidthName = 130;
constexpr int kTableColumnWidthLarge = 140;
constexpr int kTableColumnWidthXLarge = 150;
constexpr int kFormTargetStretch = 2;
constexpr int kFirewallTableStretch = 2;
constexpr int kPingMinCount = 1;
constexpr int kPingMaxCount = 1000;
constexpr int kPingDefaultCount = 10;
constexpr int kNetworkMinTimeoutMs = 100;
constexpr int kPingDefaultTimeoutMs = 4000;
constexpr int kPingDefaultIntervalMs = 1000;
constexpr int kPingMinPacketBytes = 8;
constexpr int kPingMaxPacketBytes = 65'500;
constexpr int kPingDefaultPacketBytes = 32;
constexpr int kTraceMaxHops = 64;
constexpr int kTraceDefaultHops = 30;
constexpr int kTraceDefaultTimeoutMs = 5000;
constexpr int kTraceDefaultProbeCount = 3;
constexpr int kMtrMaxCycles = 1000;
constexpr int kMtrDefaultCycles = 100;
constexpr int kMtrDefaultIntervalMs = 1000;
constexpr int kMtrDefaultTimeoutMs = 5000;
constexpr int kMtrLossWarningPercent = 5;
constexpr int kPortScanMaxTimeoutMs = 30'000;
constexpr int kPortScanDefaultTimeoutMs = 3000;
constexpr int kPortScanMaxConcurrent = 200;
constexpr int kPortScanDefaultConcurrent = 50;
constexpr int kIperfDefaultPort = 5201;
constexpr int kNetworkMaxTcpPort = 65'535;
constexpr int kBandwidthMaxDurationSec = 120;
constexpr int kBandwidthDefaultDurationSec = 10;
constexpr int kBandwidthMaxStreams = 32;
constexpr int kConnectionRefreshMinMs = 500;
constexpr int kConnectionRefreshDefaultMs = 2000;
constexpr int kLanMinPort = 1024;
constexpr int kLanDefaultPort = 5050;
constexpr int kLanMaxBlockSizeKb = 1024;
constexpr int kLanDefaultBlockSizeKb = 64;
constexpr int kNetworkSpeedBpsPerMbps = 1'000'000;
constexpr int kDefaultPingTtl = 128;
constexpr int kPortBannerPreviewChars = 200;
constexpr int kDecimalPrecisionOne = 1;
constexpr int kDecimalPrecisionTwo = 2;
constexpr double kMtrLossWarningPercentF = 5.0;
constexpr double kBitsPerByteF = 8.0;
constexpr int kMinimumBridgeAdapterCount = 2;
constexpr int kFirewallFilterOutboundIndex = 2;
constexpr int kFirewallFilterBlockIndex = 2;

enum AdapterColumn {
    KAdapterColumnName,
    KAdapterColumnType,
    KAdapterColumnStatus,
    KAdapterColumnAddress,
    KAdapterColumnMac,
    KAdapterColumnSpeed,
    KAdapterColumnCount,
};

enum PingColumn {
    KPingColumnSequence,
    KPingColumnAddress,
    KPingColumnStatus,
    KPingColumnRtt,
    KPingColumnTtl,
    KPingColumnCount,
};

enum TracerouteColumn {
    KTraceColumnHop,
    KTraceColumnAddress,
    KTraceColumnHostname,
    KTraceColumnRttFirst,
    KTraceColumnRttSecond,
    KTraceColumnRttThird,
    KTraceColumnAverage,
    KTraceColumnCount,
};

enum MtrColumn {
    KMtrColumnHop,
    KMtrColumnAddress,
    KMtrColumnLoss,
    KMtrColumnSent,
    KMtrColumnAverage,
    KMtrColumnBest,
    KMtrColumnWorst,
    KMtrColumnJitter,
    KMtrColumnCount,
};

enum DnsColumn {
    KDnsColumnQuery,
    KDnsColumnType,
    KDnsColumnServer,
    KDnsColumnResponseTime,
    KDnsColumnAnswers,
    KDnsColumnCount,
};

enum PortScanColumn {
    KPortColumnPort,
    KPortColumnState,
    KPortColumnService,
    KPortColumnResponse,
    KPortColumnBanner,
    KPortColumnCount,
};

enum ConnectionColumn {
    KConnectionColumnProtocol,
    KConnectionColumnLocalAddress,
    KConnectionColumnLocalPort,
    KConnectionColumnRemoteAddress,
    KConnectionColumnRemotePort,
    KConnectionColumnState,
    KConnectionColumnProcess,
    KConnectionColumnCount,
};

enum WifiColumn {
    KWifiColumnSsid,
    KWifiColumnBssid,
    KWifiColumnSignal,
    KWifiColumnQuality,
    KWifiColumnChannel,
    KWifiColumnBand,
    KWifiColumnSecurity,
    KWifiColumnVendor,
    KWifiColumnCount,
};

enum FirewallColumn {
    KFirewallColumnEnabled,
    KFirewallColumnName,
    KFirewallColumnDirection,
    KFirewallColumnAction,
    KFirewallColumnProtocol,
    KFirewallColumnLocalPorts,
    KFirewallColumnRemotePorts,
    KFirewallColumnApplication,
    KFirewallColumnCount,
};

enum ShareColumn {
    KShareColumnName,
    KShareColumnType,
    KShareColumnRead,
    KShareColumnWrite,
    KShareColumnRemark,
    KShareColumnCount,
};

constexpr int kPortRangePartCount = 2;
constexpr int kWifiStrongSignalDbm = -50;
constexpr int kWifiUsableSignalDbm = -70;
constexpr int kToolTabDnsIndex = 3;

}  // namespace

// ===================================================================
// Construction / Destruction
// ===================================================================

NetworkDiagnosticPanel::NetworkDiagnosticPanel(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<NetworkDiagnosticController>(this)) {
    setupUi();
    connectSignals();
    createResetNetworkAction();

    // Runtime only. Non-interactive startup gates (accessibility audit and the
    // startup smoke test) must not launch adapter enumeration side effects: the
    // scan runs on a worker thread and the gate tears the panel down moments
    // later, which would race the scan.
    const bool non_interactive_gate = qApp->property("sakAccessibilityAudit").toBool() ||
                                      qApp->property("sakStartupSmokeTest").toBool();
    if (!non_interactive_gate) {
        QMetaObject::invokeMethod(m_controller.get(),
                                  &NetworkDiagnosticController::scanAdapters,
                                  Qt::QueuedConnection);
    }
}

NetworkDiagnosticPanel::~NetworkDiagnosticPanel() {
    // Bounded-join any in-flight runCommandAsync() ops (some MUTATE adapters via netsh) so the
    // mutation does not run detached past teardown. Each op carries its own process timeout, so
    // this cannot hang teardown beyond a bounded few seconds even if several are in flight.
    for (QFuture<QPair<bool, QString>>& future : m_pending_command_futures) {
        if (future.isRunning()) {
            // SAK-ALLOW-BLOCKING: each op carries its own process timeout, so every future
            // completes on its own. Some of these MUTATE adapters via netsh; abandoning the
            // wait would let that mutation run detached past teardown.
            future.waitForFinished();
        }
    }
}

// ===================================================================
// UI Setup
// ===================================================================

void NetworkDiagnosticPanel::setupUi() {
    // Root layout -- compact header, splitter for adapter+tools, report pinned at bottom.
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    root_layout->setSpacing(ui::kSpacingSmall);

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
    root_layout->addWidget(m_toolTabs, 1);

    // Status bar with log toggle
    auto* status_row = new QHBoxLayout();
    status_row->setContentsMargins(sak::ui::kMarginNone,
                                   sak::ui::kCssPaddingTinyPx,
                                   sak::ui::kMarginNone,
                                   sak::ui::kMarginNone);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    status_row->addWidget(m_logToggle);
    status_row->addStretch();
    root_layout->addLayout(status_row);

    setupKeyboardShortcuts();
}

void NetworkDiagnosticPanel::setupKeyboardShortcuts() {
    Q_ASSERT(m_controller);
    auto* refresh_shortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
    refresh_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(
        refresh_shortcut, &QShortcut::activated, this, &NetworkDiagnosticPanel::onRefreshAdapters);

    auto* cancel_shortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    cancel_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cancel_shortcut, &QShortcut::activated, this, [this]() {
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
    auto* status_row = new QHBoxLayout();
    status_row->setContentsMargins(sak::ui::kMarginNone,
                                   sak::ui::kCssPaddingTinyPx,
                                   sak::ui::kMarginNone,
                                   sak::ui::kMarginNone);
    m_adapterLogToggle = new LogToggleSwitch(tr("Log"), widget);
    status_row->addWidget(m_adapterLogToggle);
    status_row->addStretch();
    layout->addLayout(status_row);

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
    m_backupEthernetBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_backupEthernetBtn->setToolTip(
        tr("Backup selected Ethernet adapter IP/DNS settings to a JSON file "
           "for restoration on this or another PC"));
    setAccessible(m_backupEthernetBtn,
                  tr("Backup Ethernet settings"),
                  tr("Save selected adapter's IP configuration to a portable JSON file"));
    m_backupEthernetBtn->setEnabled(false);
    toolbar->addWidget(m_backupEthernetBtn);

    m_restoreEthernetBtn = new QPushButton(tr("Restore Settings"), parent);
    m_restoreEthernetBtn->setStyleSheet(ui::kPrimaryButtonStyle);
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
    m_adapterTable->setColumnCount(KAdapterColumnCount);
    m_adapterTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Type"), tr("Status"), tr("IP Address"), tr("MAC"), tr("Speed")});
    configureStandardTable(m_adapterTable, QAbstractItemView::ExtendedSelection);
    m_adapterTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* header = m_adapterTable->horizontalHeader();
    header->setSectionResizeMode(KAdapterColumnName, QHeaderView::Stretch);
    header->setSectionResizeMode(KAdapterColumnType, QHeaderView::Interactive);
    header->resizeSection(KAdapterColumnType, kTableColumnWidthNarrow);
    header->setSectionResizeMode(KAdapterColumnStatus, QHeaderView::Interactive);
    header->resizeSection(KAdapterColumnStatus, kTableColumnWidthNarrow);
    header->setSectionResizeMode(KAdapterColumnAddress, QHeaderView::Interactive);
    header->resizeSection(KAdapterColumnAddress, kTableColumnWidthName);
    header->setSectionResizeMode(KAdapterColumnMac, QHeaderView::Interactive);
    header->resizeSection(KAdapterColumnMac, kTableColumnWidthLarge);
    header->setSectionResizeMode(KAdapterColumnSpeed, QHeaderView::Interactive);
    header->resizeSection(KAdapterColumnSpeed, kTableColumnWidthAddress);

    setAccessible(m_adapterTable,
                  tr("Network adapters"),
                  tr("List of network adapters with configuration details"));
    m_adapterTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_adapterTable, 1);

    // Designed empty/loading state (R5-G20-7). QTableWidget's model exists now, so
    // the overlay binds immediately; parented to the table so it self-manages.
    m_adapterEmptyState = new sak::ui::ViewEmptyState(
        m_adapterTable, tr("No adapters found - click Refresh Adapters"));
}

void NetworkDiagnosticPanel::setupAdapterDetailLabel(QWidget* parent, QVBoxLayout* layout) {
    Q_ASSERT(layout);
    Q_ASSERT(parent);
    const QString label_style = sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted,
                                                                   sak::ui::kFontSizeSmall);

    auto make_column = [&](QLabel*& label) -> QLabel* {
        label = new QLabel(parent);
        label->setWordWrap(true);
        label->setTextFormat(Qt::RichText);
        label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        label->setStyleSheet(label_style);
        return label;
    };

    auto* detail_row = new QHBoxLayout();
    detail_row->setSpacing(ui::kSpacingDefault);
    detail_row->setContentsMargins(sak::ui::kSpacingTight,
                                   sak::ui::kCssPaddingTinyPx,
                                   sak::ui::kSpacingTight,
                                   sak::ui::kCssPaddingTinyPx);
    detail_row->addWidget(make_column(m_detailIdentity), kFormTargetStretch);
    detail_row->addWidget(make_column(m_detailAddressing), kFormTargetStretch);
    detail_row->addWidget(make_column(m_detailGatewayDns), kFormTargetStretch);
    detail_row->addWidget(make_column(m_detailStatus), 1);

    auto* detail_widget = new QWidget(parent);
    detail_widget->setLayout(detail_row);
    detail_widget->setMinimumHeight(kNetworkDetailMinHeight);
    detail_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    layout->addWidget(detail_widget, 0);

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
    auto* target_row = new QHBoxLayout();
    target_row->addWidget(new QLabel(tr("Target:"), widget));
    m_pingTarget = new QLineEdit(widget);
    m_pingTarget->setPlaceholderText(tr("hostname or IP address"));
    m_pingTarget->setToolTip(tr("Target hostname or IP to ping"));
    setAccessible(m_pingTarget, tr("Ping target"));
    target_row->addWidget(m_pingTarget, kFormTargetStretch);
    layout->addLayout(target_row);

    // Row 2: Count, Timeout, Interval, Size
    auto* options_row = new QHBoxLayout();
    options_row->addWidget(new QLabel(tr("Count:"), widget));
    m_pingCount = new QSpinBox(widget);
    m_pingCount->setRange(kPingMinCount, kPingMaxCount);
    m_pingCount->setValue(kPingDefaultCount);
    m_pingCount->setToolTip(tr("Number of ping packets to send"));
    setAccessible(m_pingCount, tr("Ping count"), tr("Number of ICMP echo request packets to send"));
    options_row->addWidget(m_pingCount);

    options_row->addWidget(new QLabel(tr("Timeout:"), widget));
    m_pingTimeout = new QSpinBox(widget);
    m_pingTimeout->setRange(kNetworkMinTimeoutMs, kPortScanMaxTimeoutMs);
    m_pingTimeout->setValue(kPingDefaultTimeoutMs);
    m_pingTimeout->setSuffix(tr(" ms"));
    m_pingTimeout->setToolTip(tr("Timeout per ping in milliseconds"));
    setAccessible(m_pingTimeout, tr("Ping timeout"), tr("Maximum wait time for each ping reply"));
    options_row->addWidget(m_pingTimeout);

    options_row->addWidget(new QLabel(tr("Interval:"), widget));
    m_pingInterval = new QSpinBox(widget);
    m_pingInterval->setRange(kNetworkMinTimeoutMs, sak::kTimerHealthPollMs);
    m_pingInterval->setValue(kPingDefaultIntervalMs);
    m_pingInterval->setSuffix(tr(" ms"));
    m_pingInterval->setToolTip(tr("Delay between consecutive pings in milliseconds"));
    setAccessible(m_pingInterval,
                  tr("Ping interval"),
                  tr("Time between sending each ICMP echo request"));
    options_row->addWidget(m_pingInterval);

    options_row->addWidget(new QLabel(tr("Size:"), widget));
    m_pingPacketSize = new QSpinBox(widget);
    m_pingPacketSize->setRange(kPingMinPacketBytes, kPingMaxPacketBytes);
    m_pingPacketSize->setValue(kPingDefaultPacketBytes);
    m_pingPacketSize->setSuffix(tr(" B"));
    m_pingPacketSize->setToolTip(tr("ICMP packet payload size in bytes"));
    setAccessible(m_pingPacketSize, tr("Packet size"), tr("Size of the ICMP echo request payload"));
    options_row->addWidget(m_pingPacketSize);

    options_row->addStretch();
    layout->addLayout(options_row);
}

void NetworkDiagnosticPanel::setupPingControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btn_row = new QHBoxLayout();
    m_pingStartBtn = new QPushButton(tr("Start Ping"), widget);
    m_pingStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_pingStartBtn->setToolTip(tr("Send ICMP echo requests to the target host"));
    setAccessible(m_pingStartBtn, tr("Start ping test"));
    btn_row->addWidget(m_pingStartBtn);

    m_pingStopBtn = new QPushButton(tr("Stop"), widget);
    m_pingStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_pingStopBtn->setEnabled(false);
    m_pingStopBtn->setToolTip(tr("Cancel the current ping operation"));
    setAccessible(m_pingStopBtn, tr("Stop ping test"));
    btn_row->addWidget(m_pingStopBtn);

    btn_row->addStretch();
    m_pingStatsLabel = new QLabel(widget);
    m_pingStatsLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    btn_row->addWidget(m_pingStatsLabel);
    layout->addLayout(btn_row);
}

void NetworkDiagnosticPanel::setupPingResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(layout);
    Q_ASSERT(widget);
    m_pingTable = new QTableWidget(widget);
    m_pingTable->setColumnCount(KPingColumnCount);
    m_pingTable->setHorizontalHeaderLabels(
        {tr("#"), tr("IP"), tr("Status"), tr("RTT (ms)"), tr("TTL")});
    m_pingTable->setAlternatingRowColors(true);
    m_pingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pingTable->verticalHeader()->setVisible(false);

    auto* ping_header = m_pingTable->horizontalHeader();
    ping_header->setSectionResizeMode(KPingColumnSequence, QHeaderView::Interactive);
    ping_header->resizeSection(KPingColumnSequence, kTableColumnWidthTiny);
    ping_header->setSectionResizeMode(KPingColumnAddress, QHeaderView::Stretch);
    ping_header->setSectionResizeMode(KPingColumnStatus, QHeaderView::Interactive);
    ping_header->resizeSection(KPingColumnStatus, kTableColumnWidthWide);
    ping_header->setSectionResizeMode(KPingColumnRtt, QHeaderView::Interactive);
    ping_header->resizeSection(KPingColumnRtt, kTableColumnWidthWide);
    ping_header->setSectionResizeMode(KPingColumnTtl, QHeaderView::Interactive);
    ping_header->resizeSection(KPingColumnTtl, kTableColumnWidthSmall);

    setAccessible(m_pingTable, tr("Ping results"));
    m_pingTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_pingTable, 1);

    m_pingEmptyState = new sak::ui::ViewEmptyState(
        m_pingTable, tr("No ping replies yet - enter a target and Start Ping"));
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
    auto* config_row = new QHBoxLayout();
    config_row->addWidget(new QLabel(tr("Target:"), widget));
    m_traceTarget = new QLineEdit(widget);
    m_traceTarget->setPlaceholderText(tr("hostname or IP address"));
    m_traceTarget->setToolTip(tr("Target hostname or IP address to trace"));
    setAccessible(m_traceTarget, tr("Traceroute target"));
    config_row->addWidget(m_traceTarget, kFormTargetStretch);

    config_row->addWidget(new QLabel(tr("Max Hops:"), widget));
    m_traceMaxHops = new QSpinBox(widget);
    m_traceMaxHops->setRange(1, kTraceMaxHops);
    m_traceMaxHops->setValue(kTraceDefaultHops);
    m_traceMaxHops->setToolTip(tr("Maximum number of hops before giving up"));
    setAccessible(m_traceMaxHops, tr("Maximum hops"), tr("Maximum TTL value for the traceroute"));
    config_row->addWidget(m_traceMaxHops);
    layout->addLayout(config_row);
}

void NetworkDiagnosticPanel::setupTracerouteControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btn_row = new QHBoxLayout();
    m_traceStartBtn = new QPushButton(tr("Trace Route"), widget);
    m_traceStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_traceStartBtn->setToolTip(tr("Trace the network path to the target host"));
    setAccessible(m_traceStartBtn, tr("Start traceroute"));
    btn_row->addWidget(m_traceStartBtn);

    m_traceStopBtn = new QPushButton(tr("Stop"), widget);
    m_traceStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_traceStopBtn->setEnabled(false);
    m_traceStopBtn->setToolTip(tr("Cancel the current traceroute"));
    setAccessible(m_traceStopBtn, tr("Stop traceroute"));
    btn_row->addWidget(m_traceStopBtn);

    btn_row->addStretch();
    m_traceStatusLabel = new QLabel(widget);
    m_traceStatusLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    btn_row->addWidget(m_traceStatusLabel);
    layout->addLayout(btn_row);
}

void NetworkDiagnosticPanel::setupTracerouteResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_traceTable = new QTableWidget(widget);
    m_traceTable->setColumnCount(KTraceColumnCount);
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

    auto* tr_header = m_traceTable->horizontalHeader();
    tr_header->setSectionResizeMode(KTraceColumnHop, QHeaderView::Interactive);
    tr_header->resizeSection(KTraceColumnHop, kTableColumnWidthTiny);
    tr_header->setSectionResizeMode(KTraceColumnAddress, QHeaderView::Interactive);
    tr_header->resizeSection(KTraceColumnAddress, kTableColumnWidthAddress);
    tr_header->setSectionResizeMode(KTraceColumnHostname, QHeaderView::Stretch);
    for (int i = KTraceColumnRttFirst; i < KTraceColumnCount; ++i) {
        tr_header->setSectionResizeMode(i, QHeaderView::Interactive);
        tr_header->resizeSection(i, kTableColumnWidthStandard);
    }

    setAccessible(m_traceTable, tr("Traceroute results"));
    m_traceTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_traceTable, 1);

    m_traceEmptyState = new sak::ui::ViewEmptyState(
        m_traceTable, tr("No route traced yet - enter a target and Trace Route"));
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
    auto* config_row = new QHBoxLayout();
    config_row->addWidget(new QLabel(tr("Target:"), widget));
    m_mtrTarget = new QLineEdit(widget);
    m_mtrTarget->setPlaceholderText(tr("hostname or IP address"));
    m_mtrTarget->setToolTip(tr("Target hostname or IP address for MTR analysis"));
    setAccessible(m_mtrTarget, tr("MTR target"));
    config_row->addWidget(m_mtrTarget, kFormTargetStretch);

    config_row->addWidget(new QLabel(tr("Cycles:"), widget));
    m_mtrCycles = new QSpinBox(widget);
    m_mtrCycles->setRange(1, kMtrMaxCycles);
    m_mtrCycles->setValue(kMtrDefaultCycles);
    m_mtrCycles->setToolTip(tr("Number of probe cycles to run"));
    setAccessible(m_mtrCycles,
                  tr("MTR cycles"),
                  tr("Number of complete pass cycles for the MTR analysis"));
    config_row->addWidget(m_mtrCycles);
    layout->addLayout(config_row);
}

void NetworkDiagnosticPanel::setupMtrControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btn_row = new QHBoxLayout();
    m_mtrStartBtn = new QPushButton(tr("Start MTR"), widget);
    m_mtrStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_mtrStartBtn->setToolTip(tr("Start combined traceroute and ping analysis"));
    setAccessible(m_mtrStartBtn, tr("Start MTR test"));
    btn_row->addWidget(m_mtrStartBtn);

    m_mtrStopBtn = new QPushButton(tr("Stop"), widget);
    m_mtrStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_mtrStopBtn->setEnabled(false);
    m_mtrStopBtn->setToolTip(tr("Cancel the current MTR analysis"));
    setAccessible(m_mtrStopBtn, tr("Stop MTR test"));
    btn_row->addWidget(m_mtrStopBtn);

    btn_row->addStretch();
    m_mtrStatusLabel = new QLabel(widget);
    m_mtrStatusLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    btn_row->addWidget(m_mtrStatusLabel);
    layout->addLayout(btn_row);
}

void NetworkDiagnosticPanel::setupMtrResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_mtrTable = new QTableWidget(widget);
    m_mtrTable->setColumnCount(KMtrColumnCount);
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

    auto* mtr_header = m_mtrTable->horizontalHeader();
    mtr_header->setSectionResizeMode(KMtrColumnHop, QHeaderView::Interactive);
    mtr_header->resizeSection(KMtrColumnHop, kTableColumnWidthTiny);
    mtr_header->setSectionResizeMode(KMtrColumnAddress, QHeaderView::Stretch);
    for (int i = KMtrColumnLoss; i < KMtrColumnCount; ++i) {
        mtr_header->setSectionResizeMode(i, QHeaderView::Interactive);
        mtr_header->resizeSection(i, kTableColumnWidthMedium);
    }

    setAccessible(m_mtrTable, tr("MTR results"));
    m_mtrTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_mtrTable, 1);

    m_mtrEmptyState = new sak::ui::ViewEmptyState(
        m_mtrTable, tr("No MTR data yet - enter a target and Start MTR"));
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
    auto* query_row = new QHBoxLayout();
    query_row->addWidget(new QLabel(tr("Hostname:"), widget));
    m_dnsHostname = new QLineEdit(widget);
    m_dnsHostname->setPlaceholderText(tr("e.g. example.com"));
    m_dnsHostname->setToolTip(tr("Domain name or IP address to query"));
    setAccessible(m_dnsHostname, tr("DNS hostname"));
    query_row->addWidget(m_dnsHostname, kFormTargetStretch);

    query_row->addWidget(new QLabel(tr("Type:"), widget));
    m_dnsRecordType = new QComboBox(widget);
    m_dnsRecordType->addItems({"A", "AAAA", "MX", "CNAME", "TXT", "NS", "SOA", "SRV", "PTR"});
    m_dnsRecordType->setToolTip(tr("DNS record type to query"));
    setAccessible(m_dnsRecordType,
                  tr("DNS record type"),
                  tr("Select the type of DNS record to look up"));
    query_row->addWidget(m_dnsRecordType);
    layout->addLayout(query_row);

    // Row 2: DNS server
    auto* server_row = new QHBoxLayout();
    server_row->addWidget(new QLabel(tr("Server:"), widget));
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
    server_row->addWidget(m_dnsServer, 1);
    server_row->addStretch();
    layout->addLayout(server_row);
}

void NetworkDiagnosticPanel::setupDnsControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btn_row = new QHBoxLayout();
    m_dnsQueryBtn = new QPushButton(tr("Query"), widget);
    m_dnsQueryBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_dnsQueryBtn->setToolTip(tr("Perform a DNS query for the specified hostname and record type"));
    setAccessible(m_dnsQueryBtn, tr("Run DNS query"));
    btn_row->addWidget(m_dnsQueryBtn);

    m_dnsReverseBtn = new QPushButton(tr("Reverse Lookup"), widget);
    m_dnsReverseBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_dnsReverseBtn->setToolTip(tr("Resolve IP address to hostname"));
    setAccessible(m_dnsReverseBtn, tr("Reverse DNS lookup"));
    btn_row->addWidget(m_dnsReverseBtn);

    m_dnsCompareBtn = new QPushButton(tr("Compare Servers"), widget);
    m_dnsCompareBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_dnsCompareBtn->setToolTip(tr("Query multiple DNS servers and compare results"));
    setAccessible(m_dnsCompareBtn, tr("Compare DNS servers"));
    btn_row->addWidget(m_dnsCompareBtn);

    m_dnsFlushBtn = new QPushButton(tr("Flush Cache"), widget);
    m_dnsFlushBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_dnsFlushBtn->setToolTip(tr("Flush the local DNS resolver cache (requires admin)"));
    setAccessible(m_dnsFlushBtn, tr("Flush DNS cache"));
    btn_row->addWidget(m_dnsFlushBtn);

    btn_row->addStretch();
    m_dnsStatusLabel = new QLabel(widget);
    m_dnsStatusLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    btn_row->addWidget(m_dnsStatusLabel);
    layout->addLayout(btn_row);

    connect(m_dnsQueryBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsQuery);
    connect(
        m_dnsReverseBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsReverseLookup);
    connect(m_dnsCompareBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsCompare);
    connect(m_dnsFlushBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDnsFlushCache);
}

void NetworkDiagnosticPanel::setupDnsResults(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_dnsTable = new QTableWidget(widget);
    m_dnsTable->setColumnCount(KDnsColumnCount);
    m_dnsTable->setHorizontalHeaderLabels(
        {tr("Query"), tr("Type"), tr("Server"), tr("Response Time"), tr("Answers")});
    m_dnsTable->setAlternatingRowColors(true);
    m_dnsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dnsTable->verticalHeader()->setVisible(false);

    auto* dns_header = m_dnsTable->horizontalHeader();
    dns_header->setSectionResizeMode(KDnsColumnQuery, QHeaderView::Interactive);
    dns_header->resizeSection(KDnsColumnQuery, kTableColumnWidthXLarge);
    dns_header->setSectionResizeMode(KDnsColumnType, QHeaderView::Interactive);
    dns_header->resizeSection(KDnsColumnType, kTableColumnWidthSmall);
    dns_header->setSectionResizeMode(KDnsColumnServer, QHeaderView::Interactive);
    dns_header->resizeSection(KDnsColumnServer, kTableColumnWidthAddress);
    dns_header->setSectionResizeMode(KDnsColumnResponseTime, QHeaderView::Interactive);
    dns_header->resizeSection(KDnsColumnResponseTime, kTableColumnWidthWide);
    dns_header->setSectionResizeMode(KDnsColumnAnswers, QHeaderView::Stretch);

    setAccessible(m_dnsTable, tr("DNS results"));
    m_dnsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_dnsTable, 1);

    m_dnsEmptyState = new sak::ui::ViewEmptyState(
        m_dnsTable, tr("No DNS results yet - enter a hostname and Query"));
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
    auto* config_row1 = new QHBoxLayout();
    config_row1->addWidget(new QLabel(tr("Target:"), widget));
    m_portTarget = new QLineEdit(widget);
    m_portTarget->setPlaceholderText(tr("hostname or IP"));
    m_portTarget->setToolTip(tr("Target hostname or IP address to scan"));
    setAccessible(m_portTarget, tr("Port scan target"));
    config_row1->addWidget(m_portTarget, kFormTargetStretch);

    config_row1->addWidget(new QLabel(tr("Preset:"), widget));
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
    config_row1->addWidget(m_portPreset);

    config_row1->addWidget(new QLabel(tr("Custom Ports:"), widget));
    m_portCustomRange = new QLineEdit(widget);
    m_portCustomRange->setPlaceholderText(tr("e.g. 80,443,8080-8090"));
    m_portCustomRange->setToolTip(tr("Comma-separated ports or ranges (e.g. 80,443,8080-8090)"));
    setAccessible(m_portCustomRange,
                  tr("Custom port range"),
                  tr("Specify individual ports or ranges separated by commas"));
    config_row1->addWidget(m_portCustomRange, 1);
    layout->addLayout(config_row1);

    connect(m_portPreset,
            &QComboBox::currentIndexChanged,
            this,
            &NetworkDiagnosticPanel::onPortPresetChanged);

    auto* config_row2 = new QHBoxLayout();
    config_row2->addWidget(new QLabel(tr("Timeout:"), widget));
    m_portTimeout = new QSpinBox(widget);
    m_portTimeout->setRange(kNetworkMinTimeoutMs, kPortScanMaxTimeoutMs);
    m_portTimeout->setValue(kPortScanDefaultTimeoutMs);
    m_portTimeout->setSuffix(tr(" ms"));
    m_portTimeout->setToolTip(tr("Connection timeout per port in milliseconds"));
    setAccessible(m_portTimeout,
                  tr("Port scan timeout"),
                  tr("Maximum wait time for each port connection attempt"));
    config_row2->addWidget(m_portTimeout);

    config_row2->addWidget(new QLabel(tr("Concurrent:"), widget));
    m_portConcurrent = new QSpinBox(widget);
    m_portConcurrent->setRange(1, kPortScanMaxConcurrent);
    m_portConcurrent->setValue(kPortScanDefaultConcurrent);
    m_portConcurrent->setToolTip(tr("Number of ports to scan simultaneously"));
    setAccessible(m_portConcurrent,
                  tr("Concurrent scans"),
                  tr("Maximum number of parallel port connections"));
    config_row2->addWidget(m_portConcurrent);

    m_portBannerGrab = new QCheckBox(tr("Banner Grab"), widget);
    m_portBannerGrab->setChecked(true);
    m_portBannerGrab->setToolTip(tr("Attempt to read service banners on open ports"));
    setAccessible(m_portBannerGrab,
                  tr("Banner grab"),
                  tr("Read service identification banners from open ports"));
    config_row2->addWidget(m_portBannerGrab);
    config_row2->addStretch();
    layout->addLayout(config_row2);
}

void NetworkDiagnosticPanel::setupPortScanControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btn_row = new QHBoxLayout();
    m_portStartBtn = new QPushButton(tr("Scan Ports"), widget);
    m_portStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_portStartBtn->setToolTip(tr("Begin scanning the specified ports on the target"));
    setAccessible(m_portStartBtn, tr("Start port scan"));
    btn_row->addWidget(m_portStartBtn);

    m_portStopBtn = new QPushButton(tr("Stop"), widget);
    m_portStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_portStopBtn->setEnabled(false);
    m_portStopBtn->setToolTip(tr("Cancel the current port scan"));
    setAccessible(m_portStopBtn, tr("Stop port scan"));
    btn_row->addWidget(m_portStopBtn);

    btn_row->addStretch();
    m_portSummaryLabel = new QLabel(widget);
    m_portSummaryLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    btn_row->addWidget(m_portSummaryLabel);
    layout->addLayout(btn_row);
}

void NetworkDiagnosticPanel::setupPortScanResults(QWidget* widget, QVBoxLayout* layout) {
    m_portProgress = new QProgressBar(widget);
    m_portProgress->setRange(progress::kStart, progress::kComplete);
    m_portProgress->setValue(0);
    m_portProgress->setVisible(false);
    setAccessible(m_portProgress,
                  tr("Port scan progress"),
                  tr("Progress of the current port scanning operation"));
    layout->addWidget(m_portProgress);

    m_portTable = new QTableWidget(widget);
    m_portTable->setColumnCount(KPortColumnCount);
    m_portTable->setHorizontalHeaderLabels(
        {tr("Port"), tr("State"), tr("Service"), tr("Response (ms)"), tr("Banner")});
    m_portTable->setAlternatingRowColors(true);
    m_portTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_portTable->verticalHeader()->setVisible(false);
    m_portTable->setSortingEnabled(true);

    auto* port_header = m_portTable->horizontalHeader();
    port_header->setSectionResizeMode(KPortColumnPort, QHeaderView::Interactive);
    port_header->resizeSection(KPortColumnPort, kTableColumnWidthPort);
    port_header->setSectionResizeMode(KPortColumnState, QHeaderView::Interactive);
    port_header->resizeSection(KPortColumnState, kTableColumnWidthStandard);
    port_header->setSectionResizeMode(KPortColumnService, QHeaderView::Interactive);
    port_header->resizeSection(KPortColumnService, kTableColumnWidthAddress);
    port_header->setSectionResizeMode(KPortColumnResponse, QHeaderView::Interactive);
    port_header->resizeSection(KPortColumnResponse, kTableColumnWidthWide);
    port_header->setSectionResizeMode(KPortColumnBanner, QHeaderView::Stretch);

    setAccessible(m_portTable, tr("Port scan results"));
    m_portTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_portTable, 1);

    m_portEmptyState = new sak::ui::ViewEmptyState(
        m_portTable, tr("No open ports found - enter a target and Scan Ports"));
}

// -- Bandwidth Tab -------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createBandwidthTab() {
    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* widget = new QWidget(scroll_area);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    auto* iperf_group = new QGroupBox(tr("LAN Bandwidth (iPerf3)"), widget);
    auto* iperf_layout = new QVBoxLayout(iperf_group);
    setupBandwidthIperfConfig(widget, iperf_layout);
    setupBandwidthIperfControls(widget, iperf_layout);
    setupBandwidthIperfResults(widget, iperf_layout);
    layout->addWidget(iperf_group);

    setupBandwidthHttpSection(widget, layout);

    layout->addStretch();

    scroll_area->setWidget(widget);
    return scroll_area;
}

void NetworkDiagnosticPanel::setupBandwidthIperfConfig(QWidget* widget, QVBoxLayout* iperf_layout) {
    // Row 1: Server address + Port
    auto* server_row = new QHBoxLayout();
    server_row->addWidget(new QLabel(tr("Server:"), widget));
    m_bwServerAddr = new QLineEdit(widget);
    m_bwServerAddr->setPlaceholderText(tr("iPerf3 server address"));
    m_bwServerAddr->setToolTip(tr("Address of the iPerf3 server to test against"));
    setAccessible(m_bwServerAddr, tr("iPerf3 server address"));
    server_row->addWidget(m_bwServerAddr, kFormTargetStretch);

    server_row->addWidget(new QLabel(tr("Port:"), widget));
    m_bwPort = new QSpinBox(widget);
    m_bwPort->setRange(1, kNetworkMaxTcpPort);
    m_bwPort->setValue(kIperfDefaultPort);
    m_bwPort->setToolTip(tr("iPerf3 server port number"));
    setAccessible(m_bwPort, tr("iPerf3 port"), tr("TCP port for the iPerf3 server connection"));
    server_row->addWidget(m_bwPort);
    iperf_layout->addLayout(server_row);

    // Row 2: Duration + Streams + Bidirectional
    auto* options_row = new QHBoxLayout();
    options_row->addWidget(new QLabel(tr("Duration:"), widget));
    m_bwDuration = new QSpinBox(widget);
    m_bwDuration->setRange(1, kBandwidthMaxDurationSec);
    m_bwDuration->setValue(kBandwidthDefaultDurationSec);
    m_bwDuration->setSuffix(tr(" s"));
    m_bwDuration->setToolTip(tr("Duration of the bandwidth test in seconds"));
    setAccessible(m_bwDuration, tr("Test duration"), tr("How long to run the bandwidth test"));
    options_row->addWidget(m_bwDuration);

    options_row->addWidget(new QLabel(tr("Streams:"), widget));
    m_bwStreams = new QSpinBox(widget);
    m_bwStreams->setRange(1, kBandwidthMaxStreams);
    m_bwStreams->setValue(1);
    m_bwStreams->setToolTip(tr("Number of parallel streams for the test"));
    setAccessible(m_bwStreams,
                  tr("Parallel streams"),
                  tr("Number of simultaneous TCP connections for the test"));
    options_row->addWidget(m_bwStreams);

    m_bwBidirectional = new QCheckBox(tr("Bidirectional"), widget);
    m_bwBidirectional->setChecked(true);
    m_bwBidirectional->setToolTip(tr("Test both upload and download simultaneously"));
    setAccessible(m_bwBidirectional,
                  tr("Bidirectional test"),
                  tr("Run bandwidth test in both directions simultaneously"));
    options_row->addWidget(m_bwBidirectional);

    options_row->addStretch();
    iperf_layout->addLayout(options_row);
}

void NetworkDiagnosticPanel::setupBandwidthIperfControls(QWidget* widget,
                                                         QVBoxLayout* iperf_layout) {
    auto* iperf_btn_row = new QHBoxLayout();
    m_bwTestBtn = new QPushButton(tr("Run Test"), widget);
    m_bwTestBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_bwTestBtn->setToolTip(tr("Start bandwidth test against the iPerf3 server"));
    setAccessible(m_bwTestBtn, tr("Run iPerf3 bandwidth test"));
    iperf_btn_row->addWidget(m_bwTestBtn);

    m_bwServerStartBtn = new QPushButton(tr("Start Server"), widget);
    m_bwServerStartBtn->setStyleSheet(ui::kSuccessButtonStyle);
    m_bwServerStartBtn->setToolTip(
        tr("Start local iPerf3 server for other devices to test "
           "against"));
    setAccessible(m_bwServerStartBtn, tr("Start iPerf3 server"));
    iperf_btn_row->addWidget(m_bwServerStartBtn);

    m_bwServerStopBtn = new QPushButton(tr("Stop Server"), widget);
    m_bwServerStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_bwServerStopBtn->setEnabled(false);
    m_bwServerStopBtn->setToolTip(tr("Stop the local iPerf3 server"));
    setAccessible(m_bwServerStopBtn, tr("Stop iPerf3 server"));
    iperf_btn_row->addWidget(m_bwServerStopBtn);

    iperf_btn_row->addStretch();
    m_bwServerStatus = new QLabel(tr("Server: Stopped"), widget);
    m_bwServerStatus->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    iperf_btn_row->addWidget(m_bwServerStatus);
    iperf_layout->addLayout(iperf_btn_row);
}

void NetworkDiagnosticPanel::setupBandwidthIperfResults(QWidget* widget,
                                                        QVBoxLayout* iperf_layout) {
    m_bwResultLabel = new QLabel(widget);
    m_bwResultLabel->setWordWrap(true);
    m_bwResultLabel->setStyleSheet(sak::ui::fontSizeStyle(sak::ui::kFontSizeStatus));
    iperf_layout->addWidget(m_bwResultLabel);
}

void NetworkDiagnosticPanel::setupBandwidthHttpSection(QWidget* widget, QVBoxLayout* layout) {
    auto* http_group = new QGroupBox(tr("Internet Speed (HTTP)"), widget);
    auto* http_layout = new QVBoxLayout(http_group);
    auto* http_btn_row = new QHBoxLayout();
    m_httpSpeedBtn = new QPushButton(tr("Run Speed Test"), widget);
    m_httpSpeedBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_httpSpeedBtn->setToolTip(tr("Download from public CDN servers to measure internet speed"));
    setAccessible(m_httpSpeedBtn, tr("HTTP speed test"));
    http_btn_row->addWidget(m_httpSpeedBtn);
    http_btn_row->addStretch();
    http_layout->addLayout(http_btn_row);

    connect(
        m_httpSpeedBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onRunHttpSpeedTest);

    m_httpSpeedLabel = new QLabel(widget);
    m_httpSpeedLabel->setWordWrap(true);
    m_httpSpeedLabel->setStyleSheet(sak::ui::fontSizeStyle(sak::ui::kFontSizeStatus));
    http_layout->addWidget(m_httpSpeedLabel);
    layout->addWidget(http_group);
}

// -- WiFi Tab ------------------------------------------------------------

void NetworkDiagnosticPanel::setupWifiControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btn_row = new QHBoxLayout();
    m_wifiScanBtn = new QPushButton(tr("Scan WiFi"), widget);
    m_wifiScanBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_wifiScanBtn->setToolTip(tr("Scan for available WiFi networks"));
    setAccessible(m_wifiScanBtn, tr("Scan WiFi networks"));
    btn_row->addWidget(m_wifiScanBtn);
    m_wifiContBtn = new QPushButton(tr("Continuous Scan"), widget);
    m_wifiContBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    m_wifiContBtn->setToolTip(tr("Start continuous WiFi scanning for real-time monitoring"));
    setAccessible(m_wifiContBtn, tr("Start continuous WiFi scan"));
    btn_row->addWidget(m_wifiContBtn);
    m_wifiStopBtn = new QPushButton(tr("Stop"), widget);
    m_wifiStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_wifiStopBtn->setEnabled(false);
    m_wifiStopBtn->setToolTip(tr("Stop continuous WiFi scanning"));
    setAccessible(m_wifiStopBtn, tr("Stop WiFi scan"));
    btn_row->addWidget(m_wifiStopBtn);
    btn_row->addStretch();
    layout->addLayout(btn_row);
}

void NetworkDiagnosticPanel::setupWifiTable(QWidget* widget, QVBoxLayout* layout) {
    m_wifiTable = new QTableWidget(widget);
    m_wifiTable->setColumnCount(KWifiColumnCount);
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

    auto* wifi_header = m_wifiTable->horizontalHeader();
    wifi_header->setSectionResizeMode(KWifiColumnSsid, QHeaderView::Stretch);
    wifi_header->setSectionResizeMode(KWifiColumnBssid, QHeaderView::Interactive);
    wifi_header->resizeSection(KWifiColumnBssid, kTableColumnWidthName);
    wifi_header->setSectionResizeMode(KWifiColumnSignal, QHeaderView::Interactive);
    wifi_header->resizeSection(KWifiColumnSignal, kTableColumnWidthMedium);
    wifi_header->setSectionResizeMode(KWifiColumnQuality, QHeaderView::Interactive);
    wifi_header->resizeSection(KWifiColumnQuality, kTableColumnWidthMedium);
    wifi_header->setSectionResizeMode(KWifiColumnChannel, QHeaderView::Interactive);
    wifi_header->resizeSection(KWifiColumnChannel, kTableColumnWidthCompact);
    wifi_header->setSectionResizeMode(KWifiColumnBand, QHeaderView::Interactive);
    wifi_header->resizeSection(KWifiColumnBand, kTableColumnWidthSmall);
    wifi_header->setSectionResizeMode(KWifiColumnSecurity, QHeaderView::Interactive);
    wifi_header->resizeSection(KWifiColumnSecurity, kTableColumnWidthRadio);
    wifi_header->setSectionResizeMode(KWifiColumnVendor, QHeaderView::Interactive);
    wifi_header->resizeSection(KWifiColumnVendor, kTableColumnWidthWide);

    setAccessible(m_wifiTable, tr("WiFi networks"));
    m_wifiTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_wifiTable, 1);

    m_wifiEmptyState = new sak::ui::ViewEmptyState(m_wifiTable,
                                                   tr("No WiFi networks found - click Scan WiFi"));
}

void NetworkDiagnosticPanel::setupWifiStatusLabel(QWidget* widget, QVBoxLayout* layout) {
    m_wifiChannelLabel = new QLabel(widget);
    m_wifiChannelLabel->setWordWrap(true);
    m_wifiChannelLabel->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeSmall));
    layout->addWidget(m_wifiChannelLabel);
}

QWidget* NetworkDiagnosticPanel::createWiFiTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);
    setupWifiControls(widget, layout);
    setupWifiTable(widget, layout);
    setupWifiStatusLabel(widget, layout);

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
    auto* config_row = new QHBoxLayout();
    m_connShowTcp = new QCheckBox(tr("TCP"), widget);
    m_connShowTcp->setChecked(true);
    m_connShowTcp->setToolTip(tr("Show TCP connections"));
    setAccessible(m_connShowTcp, tr("Show TCP connections"));
    config_row->addWidget(m_connShowTcp);

    m_connShowUdp = new QCheckBox(tr("UDP"), widget);
    m_connShowUdp->setChecked(true);
    m_connShowUdp->setToolTip(tr("Show UDP connections"));
    setAccessible(m_connShowUdp, tr("Show UDP connections"));
    config_row->addWidget(m_connShowUdp);

    config_row->addWidget(new QLabel(tr("Process:"), widget));
    m_connProcessFilter = new QLineEdit(widget);
    m_connProcessFilter->setPlaceholderText(tr("filter by process name"));
    m_connProcessFilter->setClearButtonEnabled(true);
    m_connProcessFilter->setToolTip(tr("Filter connections by process name"));
    setAccessible(m_connProcessFilter, tr("Process filter"));
    config_row->addWidget(m_connProcessFilter, 1);

    config_row->addWidget(new QLabel(tr("Refresh:"), widget));
    m_connRefreshRate = new QSpinBox(widget);
    m_connRefreshRate->setRange(kConnectionRefreshMinMs, kPortScanMaxTimeoutMs);
    m_connRefreshRate->setValue(kConnectionRefreshDefaultMs);
    m_connRefreshRate->setSuffix(tr(" ms"));
    m_connRefreshRate->setToolTip(tr("How often to refresh the connection list"));
    setAccessible(m_connRefreshRate,
                  tr("Refresh rate"),
                  tr("Interval between connection list updates"));
    config_row->addWidget(m_connRefreshRate);
    layout->addLayout(config_row);
}

void NetworkDiagnosticPanel::setupConnectionsControls(QWidget* widget, QVBoxLayout* layout) {
    auto* btn_row = new QHBoxLayout();
    m_connStartBtn = new QPushButton(tr("Start Monitor"), widget);
    m_connStartBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_connStartBtn->setToolTip(tr("Start monitoring active network connections"));
    setAccessible(m_connStartBtn, tr("Start connection monitor"));
    btn_row->addWidget(m_connStartBtn);

    m_connStopBtn = new QPushButton(tr("Stop"), widget);
    m_connStopBtn->setStyleSheet(ui::kDangerButtonStyle);
    m_connStopBtn->setEnabled(false);
    m_connStopBtn->setToolTip(tr("Stop monitoring connections"));
    setAccessible(m_connStopBtn, tr("Stop connection monitor"));
    btn_row->addWidget(m_connStopBtn);

    btn_row->addStretch();
    m_connSummaryLabel = new QLabel(widget);
    m_connSummaryLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    btn_row->addWidget(m_connSummaryLabel);
    layout->addLayout(btn_row);
}

void NetworkDiagnosticPanel::setupConnectionsTable(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_connTable = new QTableWidget(widget);
    m_connTable->setColumnCount(KConnectionColumnCount);
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

    auto* conn_header = m_connTable->horizontalHeader();
    conn_header->setSectionResizeMode(KConnectionColumnProtocol, QHeaderView::Interactive);
    conn_header->resizeSection(KConnectionColumnProtocol, kTableColumnWidthCompact);
    conn_header->setSectionResizeMode(KConnectionColumnLocalAddress, QHeaderView::Interactive);
    conn_header->resizeSection(KConnectionColumnLocalAddress, kTableColumnWidthAddress);
    conn_header->setSectionResizeMode(KConnectionColumnLocalPort, QHeaderView::Interactive);
    conn_header->resizeSection(KConnectionColumnLocalPort, kTableColumnWidthMedium);
    conn_header->setSectionResizeMode(KConnectionColumnRemoteAddress, QHeaderView::Interactive);
    conn_header->resizeSection(KConnectionColumnRemoteAddress, kTableColumnWidthAddress);
    conn_header->setSectionResizeMode(KConnectionColumnRemotePort, QHeaderView::Interactive);
    conn_header->resizeSection(KConnectionColumnRemotePort, kTableColumnWidthRemotePort);
    conn_header->setSectionResizeMode(KConnectionColumnState, QHeaderView::Interactive);
    conn_header->resizeSection(KConnectionColumnState, kTableColumnWidthState);
    conn_header->setSectionResizeMode(KConnectionColumnProcess, QHeaderView::Stretch);

    setAccessible(m_connTable, tr("Active connections"));
    m_connTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_connTable, 1);

    m_connEmptyState =
        new sak::ui::ViewEmptyState(m_connTable, tr("No active connections - click Start Monitor"));
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
    m_fwSummaryLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    toolbar->addWidget(m_fwSummaryLabel);
    layout->addLayout(toolbar);
}

void NetworkDiagnosticPanel::setupFirewallRuleTable(QWidget* widget, QVBoxLayout* layout) {
    Q_ASSERT(widget);
    m_fwRuleTable = new QTableWidget(widget);
    m_fwRuleTable->setColumnCount(KFirewallColumnCount);
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

    auto* fw_header = m_fwRuleTable->horizontalHeader();
    fw_header->setSectionResizeMode(KFirewallColumnEnabled, QHeaderView::Interactive);
    fw_header->resizeSection(KFirewallColumnEnabled, kTableColumnWidthSmall);
    fw_header->setSectionResizeMode(KFirewallColumnName, QHeaderView::Stretch);
    fw_header->setSectionResizeMode(KFirewallColumnDirection, QHeaderView::Interactive);
    fw_header->resizeSection(KFirewallColumnDirection, kTableColumnWidthStandard);
    fw_header->setSectionResizeMode(KFirewallColumnAction, QHeaderView::Interactive);
    fw_header->resizeSection(KFirewallColumnAction, kTableColumnWidthSmall);
    fw_header->setSectionResizeMode(KFirewallColumnProtocol, QHeaderView::Interactive);
    fw_header->resizeSection(KFirewallColumnProtocol, kTableColumnWidthPort);
    fw_header->setSectionResizeMode(KFirewallColumnLocalPorts, QHeaderView::Interactive);
    fw_header->resizeSection(KFirewallColumnLocalPorts, kTableColumnWidthWide);
    fw_header->setSectionResizeMode(KFirewallColumnRemotePorts, QHeaderView::Interactive);
    fw_header->resizeSection(KFirewallColumnRemotePorts, kTableColumnWidthWide);
    fw_header->setSectionResizeMode(KFirewallColumnApplication, QHeaderView::Interactive);
    fw_header->resizeSection(KFirewallColumnApplication, kTableColumnWidthXLarge);

    setAccessible(m_fwRuleTable, tr("Firewall rules"));
    m_fwRuleTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_fwRuleTable, kFirewallTableStretch);

    m_fwRuleEmptyState = new sak::ui::ViewEmptyState(
        m_fwRuleTable, tr("No firewall rules to show - run Full Audit or clear filters"));
}

void NetworkDiagnosticPanel::setupFirewallAnalysis(QWidget* widget, QVBoxLayout* layout) {
    auto* analysis_row = new QHBoxLayout();

    auto* conflict_group = new QGroupBox(tr("Conflicts"), widget);
    auto* conflict_layout = new QVBoxLayout(conflict_group);
    m_fwConflictText = new QTextEdit(widget);
    m_fwConflictText->setReadOnly(true);
    m_fwConflictText->setMaximumHeight(kFirewallAnalysisMaxHeight);
    setAccessible(m_fwConflictText,
                  tr("Firewall conflicts"),
                  tr("Detected firewall rule conflicts"));
    conflict_layout->addWidget(m_fwConflictText);
    analysis_row->addWidget(conflict_group);

    auto* gap_group = new QGroupBox(tr("Coverage Gaps"), widget);
    auto* gap_layout = new QVBoxLayout(gap_group);
    m_fwGapText = new QTextEdit(widget);
    m_fwGapText->setReadOnly(true);
    m_fwGapText->setMaximumHeight(kFirewallAnalysisMaxHeight);
    setAccessible(m_fwGapText, tr("Coverage gaps"), tr("Detected firewall coverage gaps"));
    gap_layout->addWidget(m_fwGapText);
    analysis_row->addWidget(gap_group);

    layout->addLayout(analysis_row);
}

// -- Shares Tab ----------------------------------------------------------

QWidget* NetworkDiagnosticPanel::createSharesTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    auto* config_row = new QHBoxLayout();
    config_row->addWidget(new QLabel(tr("Hostname:"), widget));
    m_shareHostname = new QLineEdit(widget);
    m_shareHostname->setPlaceholderText(tr("target hostname or IP (blank = local)"));
    m_shareHostname->setToolTip(tr("Leave blank to discover shares on the local machine"));
    setAccessible(m_shareHostname, tr("Share discovery hostname"));
    config_row->addWidget(m_shareHostname, kFormTargetStretch);

    m_shareDiscoverBtn = new QPushButton(tr("Discover Shares"), widget);
    m_shareDiscoverBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_shareDiscoverBtn->setToolTip(tr("Discover shared folders and resources on the target host"));
    setAccessible(m_shareDiscoverBtn, tr("Discover network shares"));
    config_row->addWidget(m_shareDiscoverBtn);
    layout->addLayout(config_row);

    connect(
        m_shareDiscoverBtn, &QPushButton::clicked, this, &NetworkDiagnosticPanel::onDiscoverShares);

    m_shareTable = new QTableWidget(widget);
    m_shareTable->setColumnCount(KShareColumnCount);
    m_shareTable->setHorizontalHeaderLabels(
        {tr("Share Name"), tr("Type"), tr("Read"), tr("Write"), tr("Remark")});
    m_shareTable->setAlternatingRowColors(true);
    m_shareTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_shareTable->verticalHeader()->setVisible(false);

    auto* share_header = m_shareTable->horizontalHeader();
    share_header->setSectionResizeMode(KShareColumnName, QHeaderView::Stretch);
    share_header->setSectionResizeMode(KShareColumnType, QHeaderView::Interactive);
    share_header->resizeSection(KShareColumnType, kTableColumnWidthStandard);
    share_header->setSectionResizeMode(KShareColumnRead, QHeaderView::Interactive);
    share_header->resizeSection(KShareColumnRead, kTableColumnWidthSmall);
    share_header->setSectionResizeMode(KShareColumnWrite, QHeaderView::Interactive);
    share_header->resizeSection(KShareColumnWrite, kTableColumnWidthSmall);
    share_header->setSectionResizeMode(KShareColumnRemark, QHeaderView::Stretch);

    setAccessible(m_shareTable, tr("Network shares"));
    m_shareTable->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_shareTable, 1);

    m_shareEmptyState = new sak::ui::ViewEmptyState(
        m_shareTable, tr("No shares discovered - click Discover Shares"));

    return widget;
}

// -- LAN Transfer Tab -- Group Builders ------------------------------------

QGroupBox* NetworkDiagnosticPanel::createLanServerGroup(QWidget* parent) {
    auto* group = new QGroupBox(tr("LAN Transfer Server (Receiver)"), parent);
    auto* group_layout = new QVBoxLayout(group);
    auto* row = new QHBoxLayout();

    row->addWidget(new QLabel(tr("Listen Port:"), parent));
    m_lanPort = new QSpinBox(parent);
    m_lanPort->setRange(kLanMinPort, kNetworkMaxTcpPort);
    m_lanPort->setValue(kLanDefaultPort);
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
    m_lanServerStatus->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    row->addWidget(m_lanServerStatus);
    group_layout->addLayout(row);

    return group;
}

QGroupBox* NetworkDiagnosticPanel::createLanClientGroup(QWidget* parent) {
    auto* group = new QGroupBox(tr("LAN Transfer Client (Sender)"), parent);
    auto* group_layout = new QVBoxLayout(group);

    auto* target_row = new QHBoxLayout();
    target_row->addWidget(new QLabel(tr("Target:"), parent));
    m_lanTarget = new QLineEdit(parent);
    m_lanTarget->setPlaceholderText(tr("IP address of the receiving device"));
    m_lanTarget->setToolTip(
        tr("Enter the IP address of the device running "
           "the LAN Transfer server"));
    setAccessible(m_lanTarget, tr("LAN transfer target address"));
    target_row->addWidget(m_lanTarget, kFormTargetStretch);
    group_layout->addLayout(target_row);

    auto* opt_row = new QHBoxLayout();
    opt_row->addWidget(new QLabel(tr("Duration:"), parent));
    m_lanDuration = new QSpinBox(parent);
    m_lanDuration->setRange(1, kBandwidthMaxDurationSec);
    m_lanDuration->setValue(kBandwidthDefaultDurationSec);
    m_lanDuration->setSuffix(tr(" s"));
    m_lanDuration->setToolTip(tr("How long to send data, in seconds"));
    setAccessible(m_lanDuration, tr("LAN transfer test duration"));
    opt_row->addWidget(m_lanDuration);

    opt_row->addWidget(new QLabel(tr("Block Size:"), parent));
    m_lanBlockSize = new QSpinBox(parent);
    m_lanBlockSize->setRange(1, kLanMaxBlockSizeKb);
    m_lanBlockSize->setValue(kLanDefaultBlockSizeKb);
    m_lanBlockSize->setSuffix(tr(" KB"));
    m_lanBlockSize->setToolTip(
        tr("Size of each data block sent "
           "(larger may improve throughput)"));
    setAccessible(m_lanBlockSize, tr("LAN transfer block size"));
    opt_row->addWidget(m_lanBlockSize);
    opt_row->addStretch();
    group_layout->addLayout(opt_row);

    auto* btn_row = new QHBoxLayout();
    m_lanTestBtn = new QPushButton(tr("Run Transfer Test"), parent);
    m_lanTestBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    m_lanTestBtn->setToolTip(
        tr("Send data to the target device and measure "
           "transfer speed"));
    setAccessible(m_lanTestBtn, tr("Run LAN transfer speed test"));
    btn_row->addWidget(m_lanTestBtn);
    btn_row->addStretch();
    group_layout->addLayout(btn_row);

    return group;
}

// -- LAN Transfer Tab ----------------------------------------------------

QWidget* NetworkDiagnosticPanel::createLanTransferTab() {
    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* widget = new QWidget(scroll_area);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    layout->addWidget(createLanServerGroup(widget));
    layout->addWidget(createLanClientGroup(widget));

    m_lanResultLabel = new QLabel(widget);
    m_lanResultLabel->setWordWrap(true);
    m_lanResultLabel->setStyleSheet(sak::ui::fontSizeStyle(sak::ui::kFontSizeStatus));
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

    scroll_area->setWidget(widget);
    return scroll_area;
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
    connectAdapterUiSignals();
    connectProbeUiSignals();
    connectBandwidthWifiUiSignals();
    connectConnectionsContextSignals();
}

void NetworkDiagnosticPanel::connectAdapterUiSignals() {
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
}

void NetworkDiagnosticPanel::connectProbeUiSignals() {
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
}

void NetworkDiagnosticPanel::connectBandwidthWifiUiSignals() {
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
}

void NetworkDiagnosticPanel::connectConnectionsContextSignals() {
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
            &NetworkDiagnosticController::operationFinished,
            this,
            &NetworkDiagnosticPanel::onOperationFinished);
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
        Q_EMIT statusMessage(tr("DNS cache flushed"), sak::kTimerStatusMessageMs);
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
            [this](double current_mbps, double elapsed_sec, double total_sec) {
                m_bwResultLabel->setText(QStringLiteral("Running: %1 Mbps (%2/%3 s)")
                                             .arg(current_mbps, 0, 'f', 1)
                                             .arg(elapsed_sec, 0, 'f', 0)
                                             .arg(total_sec, 0, 'f', 0));
            });
    connect(m_controller.get(),
            &NetworkDiagnosticController::progressUpdated,
            this,
            [this](int percent, QString status) {
                Q_EMIT statusMessage(QStringLiteral("%1 (%2%)").arg(status).arg(percent),
                                     sak::kTimerBroadcastMs);
                Q_EMIT progressUpdate(percent, progress::kComplete);
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
                Q_EMIT statusMessage(QStringLiteral("Report saved to %1").arg(path),
                                     sak::kTimerStatusDefaultMs);
                sak::showInformationLogged(this,
                                           tr("Report Generated"),
                                           QStringLiteral("Report saved to:\n%1").arg(path));
            });
    connect(m_controller.get(),
            &NetworkDiagnosticController::ethernetBackupComplete,
            this,
            [this](QString path) {
                sak::showInformationLogged(this,
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
                    sak::showInformationLogged(this,
                                               tr("Restore Complete"),
                                               tr("Ethernet settings restored successfully.\n\n"
                                                  "The adapter may take a moment to apply the new "
                                                  "configuration."));
                } else {
                    sak::logWarning("Ethernet settings restore incomplete");
                    sak::showWarningLogged(this,
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
    m_adapterEmptyState->setLoading(tr("Enumerating network adapters..."));
    m_controller->scanAdapters();
}

void NetworkDiagnosticPanel::onAdaptersScanComplete(QVector<NetworkAdapterInfo> adapters) {
    Q_ASSERT(m_adapterTable);
    m_adapterEmptyState->clearLoading();
    m_adapters = adapters;
    m_adapterTable->setRowCount(0);
    m_adapterTable->setSortingEnabled(false);

    for (const auto& a : adapters) {
        const int row = m_adapterTable->rowCount();
        m_adapterTable->insertRow(row);

        auto* name_item = new QTableWidgetItem(a.name);
        name_item->setData(Qt::UserRole, row);  // Store original data index
        m_adapterTable->setItem(row, 0, name_item);
        m_adapterTable->setItem(row, 1, new QTableWidgetItem(a.adapterType));

        auto* status_item = new QTableWidgetItem(a.isConnected ? tr("Connected")
                                                               : tr("Disconnected"));
        status_item->setForeground(a.isConnected ? QColor(ui::kColorSuccess)
                                                 : QColor(ui::kColorError));
        m_adapterTable->setItem(row, KAdapterColumnStatus, status_item);

        const auto ip = a.ipv4Addresses.isEmpty() ? QStringLiteral("--") : a.ipv4Addresses.first();
        m_adapterTable->setItem(row, KAdapterColumnAddress, new QTableWidgetItem(ip));
        m_adapterTable->setItem(row, KAdapterColumnMac, new QTableWidgetItem(a.macAddress));

        const auto speed = a.linkSpeedBps > 0 ? QStringLiteral("%1 Mbps").arg(
                                                    a.linkSpeedBps / kNetworkSpeedBpsPerMbps)
                                              : QStringLiteral("--");
        m_adapterTable->setItem(row, KAdapterColumnSpeed, new QTableWidgetItem(speed));
    }

    m_adapterTable->setSortingEnabled(true);
    Q_EMIT statusMessage(QStringLiteral("%1 adapters found").arg(adapters.size()),
                         sak::kTimerStatusMessageMs);
}

QString NetworkDiagnosticPanel::formatAdapterIdentity(const NetworkAdapterInfo& adapter) const {
    // The adapter alias is renameable by any local user and the description is a driver INF
    // string; this feeds m_detailIdentity, which is explicitly Qt::RichText, so escape both.
    return QStringLiteral("<b>%1</b><br>%2<br>MAC: %3")
        .arg(adapter.name.toHtmlEscaped(),
             adapter.description.toHtmlEscaped(),
             adapter.macAddress.toHtmlEscaped());
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
        text +=
            QStringLiteral("Speed: %1 Mbps").arg(adapter.linkSpeedBps / kNetworkSpeedBpsPerMbps);
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

    const auto* name_item = m_adapterTable->item(row, 0);
    if (name_item == nullptr) {
        return;
    }
    const int data_idx = name_item->data(Qt::UserRole).toInt();
    if (data_idx < 0 || data_idx >= m_adapters.size()) {
        return;
    }

    const auto& a = m_adapters[data_idx];
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
    config += QStringLiteral("Speed: %1 Mbps\n").arg(a.linkSpeedBps / kNetworkSpeedBpsPerMbps);

    QApplication::clipboard()->setText(config);
    Q_EMIT statusMessage(tr("Adapter configuration copied to clipboard"),
                         sak::kTimerStatusMessageMs);
}

void NetworkDiagnosticPanel::onBackupEthernetSettings() {
    Q_ASSERT(m_adapterTable);
    Q_ASSERT(m_controller);
    const int row = m_adapterTable->currentRow();
    if (row < 0) {
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

    const QString file_path =
        QFileDialog::getSaveFileName(this,
                                     tr("Save Ethernet Settings Backup"),
                                     QStringLiteral("%1_ethernet_backup.json").arg(adapter.name),
                                     tr("JSON Files (*.json);;All Files (*.*)"));

    if (file_path.isEmpty()) {
        return;
    }

    m_controller->backupEthernetSettings(adapter.name, file_path);
}

void NetworkDiagnosticPanel::onRestoreEthernetSettings() {
    Q_ASSERT(m_controller);
    QString file_path = QFileDialog::getOpenFileName(this,
                                                     tr("Open Ethernet Settings Backup"),
                                                     QString(),
                                                     tr("JSON Files (*.json);;All Files (*.*)"));

    if (file_path.isEmpty()) {
        return;
    }

    // Let user choose target adapter
    QStringList adapters = m_controller->listEthernetAdapters();
    if (adapters.isEmpty()) {
        sak::logWarning("No Ethernet adapters found for settings restore");
        sak::showWarningLogged(this,
                               tr("No Adapters"),
                               tr("No Ethernet adapters found on this system."));
        return;
    }

    // If there's only one adapter, use it directly
    QString target_adapter;
    if (adapters.size() == 1) {
        target_adapter = adapters.first();
    } else {
        // Show selection dialog
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Select Target Adapter"));
        dialog.setMinimumWidth(kAdapterSelectDialogMinWidth);

        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(
            tr("Select the Ethernet adapter to apply the backup settings to:"), &dialog));

        auto* adapter_combo = new QComboBox(&dialog);
        for (const auto& name : adapters) {
            adapter_combo->addItem(name);
        }
        layout->addWidget(adapter_combo);

        auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                &dialog);
        layout->addWidget(button_box);

        connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        target_adapter = adapter_combo->currentText();
    }

    auto result =
        sak::showWarningLogged(this,
                               tr("Confirm Restore"),
                               tr("This will change the IP configuration of <b>%1</b>.\n\n"
                                  "Current settings will be overwritten. You may lose network "
                                  "connectivity temporarily.\n\n"
                                  "Administrator privileges are required.\n\n"
                                  "Continue?")
                                   .arg(target_adapter.toHtmlEscaped()),
                               QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No);

    if (result == QMessageBox::Yes) {
        m_controller->restoreEthernetSettings(file_path, target_adapter);
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
    if (name_item == nullptr) {
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
        if (name_item == nullptr) {
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
    constexpr int kFinishTimeoutMs = 15'000;
    // System32-qualified netsh, never the bare name: CreateProcess searches the current
    // directory ahead of System32, and these adapter commands are privileged. Fail closed
    // when the path cannot be resolved rather than run whatever PATH/CWD supplies.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        if (output != nullptr) {
            *output = tr("Cannot resolve the System32 netsh.exe path");
        }
        return false;
    }
    const auto process = runProcess(netsh_exe, args, kFinishTimeoutMs);

    if (process.timed_out) {
        if (output != nullptr) {
            *output = tr("netsh command timed out");
        }
        return false;
    }

    if (output != nullptr) {
        *output = process.std_out.isEmpty() ? process.std_err : process.std_out;
    }
    return process.exit_code == 0;
}

void NetworkDiagnosticPanel::runCommandAsync(
    const QString& program,
    const QStringList& args,
    int timeout_ms,
    std::function<void(bool success, QString output)> callback) {
    auto* watcher = new QFutureWatcher<QPair<bool, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<bool, QString>>::finished, this, [watcher, callback]() {
        const auto [success, output] = watcher->result();
        watcher->deleteLater();
        callback(success, output);
    });
    const QFuture<QPair<bool, QString>> future =
        QtConcurrent::run([program, args, timeout_ms]() -> QPair<bool, QString> {
            const auto process = runProcess(program, args, timeout_ms);
            const QString output = process.std_out.isEmpty() ? process.std_err : process.std_out;
            if (process.timed_out) {
                return {false, QStringLiteral("%1 command timed out").arg(program)};
            }
            return {process.exit_code == 0, output};
        });
    watcher->setFuture(future);
    // Track the future so the destructor can bounded-wait a still-running (possibly MUTATING) op
    // rather than let it run detached past teardown. Prune finished ones first so this stays
    // bounded.
    m_pending_command_futures.removeIf(
        [](const QFuture<QPair<bool, QString>>& f) { return f.isFinished(); });
    m_pending_command_futures.append(future);
}

void NetworkDiagnosticPanel::runNetshCommandAsync(
    const QStringList& args, std::function<void(bool success, QString output)> callback) {
    // System32-qualified netsh only; an unresolvable path reports failure through the
    // caller's own callback instead of launching a PATH/CWD-resolved binary.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        callback(false, tr("Cannot resolve the System32 netsh.exe path"));
        return;
    }
    runCommandAsync(netsh_exe, args, sak::kTimeoutNetworkReadMs, std::move(callback));
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
    const bool is_ip_configurable =
        static_cast<bool>(static_cast<int>(is_ethernet) | static_cast<int>(is_wifi));
    const bool can_rename =
        static_cast<bool>(static_cast<int>(!is_loopback) & static_cast<int>(!is_vpn));

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
    if (adapter == nullptr) {
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

    const auto speed_text =
        adapter.linkSpeedBps > 0
            ? QStringLiteral("%1 Mbps").arg(adapter.linkSpeedBps / kNetworkSpeedBpsPerMbps)
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
    const NetworkAdapterInfo* selected = selectedAdapter();
    if (selected == nullptr) {
        return;
    }
    // Snapshot the adapter by value before the nested event loop below (dialog or message
    // box): a queued adapter rescan delivered while it runs reassigns m_adapters and frees
    // what selectedAdapter() returned, so a raw pointer would dangle (use-after-free).
    const NetworkAdapterInfo adapter_snapshot = *selected;
    const NetworkAdapterInfo* adapter = &adapter_snapshot;

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
    if (adapter == nullptr) {
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

    // explorer.exe by absolute Windows-directory path (it is NOT a System32 binary): a
    // shell-open is resolved through the same search order as any other launch, so a bare
    // name would let a planted explorer.exe run. Unresolvable -> nothing is launched.
    if (!sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"), {settings_uri})) {
        Q_EMIT logOutput(tr("[ERROR] Could not launch Windows Settings"));
        return;
    }
    Q_EMIT logOutput(tr("Opened %1 properties for '%2'").arg(type, adapter->name));
    Q_EMIT statusMessage(tr("Opened %1 settings for '%2'").arg(type, adapter->name),
                         sak::kTimerStatusMessageMs);
    sak::logInfo("Opened properties for {} adapter '{}'",
                 type.toStdString(),
                 adapter->name.toStdString());
}

void NetworkDiagnosticPanel::onAdapterEnable() {
    const auto* adapter = selectedAdapter();
    if (adapter == nullptr) {
        return;
    }

    Q_EMIT logOutput(tr("Enabling adapter '%1'...").arg(adapter->name));

    const QStringList args = {QStringLiteral("interface"),
                              QStringLiteral("set"),
                              QStringLiteral("interface"),
                              adapter->name,
                              QStringLiteral("admin=ENABLED")};
    const QString adapter_name = adapter->name;
    runNetshCommandAsync(args, [this, adapter_name](bool success, const QString& output) {
        if (success) {
            Q_EMIT statusMessage(tr("Adapter '%1' enabled").arg(adapter_name),
                                 sak::kTimerStatusMessageMs);
            Q_EMIT logOutput(tr("Adapter '%1' enabled successfully").arg(adapter_name));
            sak::logInfo("Enabled adapter: {}", adapter_name.toStdString());
        } else {
            sak::logError("Failed to enable adapter {}: {}",
                          adapter_name.toStdString(),
                          output.toStdString());
            Q_EMIT logOutput(tr("[ERROR] Failed to enable '%1': %2").arg(adapter_name, output));
            sak::showWarningLogged(this,
                                   tr("Enable Failed"),
                                   tr("Failed to enable adapter.\n\n"
                                      "Administrator privileges may be required.\n\n%1")
                                       .arg(output));
        }

        constexpr int kRefreshDelayMs = 2000;
        QTimer::singleShot(kRefreshDelayMs, this, &NetworkDiagnosticPanel::onRefreshAdapters);
    });
}

void NetworkDiagnosticPanel::onAdapterDisable() {
    const NetworkAdapterInfo* selected = selectedAdapter();
    if (selected == nullptr) {
        return;
    }
    // Snapshot the adapter by value before the nested event loop below (dialog or message
    // box): a queued adapter rescan delivered while it runs reassigns m_adapters and frees
    // what selectedAdapter() returned, so a raw pointer would dangle (use-after-free).
    const NetworkAdapterInfo adapter_snapshot = *selected;
    const NetworkAdapterInfo* adapter = &adapter_snapshot;

    auto confirm = sak::showQuestionLogged(this,
                                           tr("Disable Adapter"),
                                           tr("Disable adapter <b>%1</b>?\n\n"
                                              "You may lose network connectivity.")
                                               .arg(adapter->name.toHtmlEscaped()),
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    Q_EMIT logOutput(tr("Disabling adapter '%1'...").arg(adapter->name));

    const QStringList args = {QStringLiteral("interface"),
                              QStringLiteral("set"),
                              QStringLiteral("interface"),
                              adapter->name,
                              QStringLiteral("admin=DISABLED")};
    const QString adapter_name = adapter->name;
    runNetshCommandAsync(args, [this, adapter_name](bool success, const QString& output) {
        if (success) {
            Q_EMIT statusMessage(tr("Adapter '%1' disabled").arg(adapter_name),
                                 sak::kTimerStatusMessageMs);
            Q_EMIT logOutput(tr("Adapter '%1' disabled successfully").arg(adapter_name));
            sak::logInfo("Disabled adapter: {}", adapter_name.toStdString());
        } else {
            sak::logError("Failed to disable adapter {}: {}",
                          adapter_name.toStdString(),
                          output.toStdString());
            Q_EMIT logOutput(tr("[ERROR] Failed to disable '%1': %2").arg(adapter_name, output));
            sak::showWarningLogged(this,
                                   tr("Disable Failed"),
                                   tr("Failed to disable adapter.\n\n"
                                      "Administrator privileges may be required.\n\n%1")
                                       .arg(output));
        }

        constexpr int kRefreshDelayMs = 2000;
        QTimer::singleShot(kRefreshDelayMs, this, &NetworkDiagnosticPanel::onRefreshAdapters);
    });
}

void NetworkDiagnosticPanel::onAdapterDiagnose() {
    const auto* adapter = selectedAdapter();
    if (adapter == nullptr) {
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

    // System32-qualified msdt.exe; unresolvable (or absent on newer Windows) -> report the
    // failure rather than let CreateProcess search PATH/CWD for the name.
    if (!sak::startDetachedSystem32Tool(QStringLiteral("msdt.exe"),
                                        {QStringLiteral("/id"), diagnostic_id})) {
        Q_EMIT logOutput(tr("[ERROR] Could not launch the Windows network troubleshooter"));
        return;
    }
    Q_EMIT statusMessage(tr("Running %1 diagnostics for '%2'...").arg(type, adapter->name),
                         sak::kTimerStatusMessageMs);
    Q_EMIT logOutput(tr("Launched %1 diagnostics for '%2'").arg(type, adapter->name));
    sak::logInfo("Launched diagnostics ({}) for adapter '{}'",
                 diagnostic_id.toStdString(),
                 adapter->name.toStdString());
}

void NetworkDiagnosticPanel::onAdapterRename() {
    const NetworkAdapterInfo* selected = selectedAdapter();
    if (selected == nullptr) {
        return;
    }
    // Snapshot the adapter by value before the nested event loop below (dialog or message
    // box): a queued adapter rescan delivered while it runs reassigns m_adapters and frees
    // what selectedAdapter() returned, so a raw pointer would dangle (use-after-free).
    const NetworkAdapterInfo adapter_snapshot = *selected;
    const NetworkAdapterInfo* adapter = &adapter_snapshot;

    constexpr int kRenameDialogWidth = 400;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Rename Adapter"));
    dialog.setMinimumWidth(kRenameDialogWidth);
    auto* layout = new QVBoxLayout(&dialog);

    layout->addWidget(
        new QLabel(tr("Current name: <b>%1</b>").arg(adapter->name.toHtmlEscaped()), &dialog));

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

    const QStringList args = {QStringLiteral("interface"),
                              QStringLiteral("set"),
                              QStringLiteral("interface"),
                              adapter->name,
                              QStringLiteral("newname=") + new_name};
    const QString old_name = adapter->name;
    runNetshCommandAsync(args, [this, old_name, new_name](bool success, const QString& output) {
        if (success) {
            Q_EMIT statusMessage(tr("Adapter renamed to '%1'").arg(new_name),
                                 sak::kTimerStatusMessageMs);
            Q_EMIT logOutput(tr("Adapter renamed to '%1'").arg(new_name));
            sak::logInfo("Renamed adapter '{}' to '{}'",
                         old_name.toStdString(),
                         new_name.toStdString());
            onRefreshAdapters();
        } else {
            sak::logError("Failed to rename adapter: {}", output.toStdString());
            Q_EMIT logOutput(tr("[ERROR] Failed to rename: %1").arg(output));
            sak::showWarningLogged(this,
                                   tr("Rename Failed"),
                                   tr("Failed to rename adapter.\n\n"
                                      "Administrator privileges may be required.\n\n%1")
                                       .arg(output));
        }
    });
}

void NetworkDiagnosticPanel::onOpenAdapterSettings() {
    const auto* adapter = selectedAdapter();
    if (adapter == nullptr) {
        return;
    }

    // Absolute paths throughout: explorer.exe from the Windows directory, control.exe from
    // System32 (and by its real ".exe" name). A failed resolve launches nothing.
    const auto& type = adapter->adapterType;
    bool launched = false;
    if (type == QStringLiteral("WiFi")) {
        launched = sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"),
                                                 {QStringLiteral("ms-settings:network-wifi")});
    } else if (type == QStringLiteral("Bluetooth")) {
        launched = sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"),
                                                 {QStringLiteral("ms-settings:bluetooth")});
    } else if (type == QStringLiteral("VPN")) {
        launched = sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"),
                                                 {QStringLiteral("ms-settings:network-vpn")});
    } else {
        launched = sak::startDetachedSystem32Tool(QStringLiteral("control.exe"),
                                                  {QStringLiteral("ncpa.cpl")});
    }
    if (!launched) {
        Q_EMIT logOutput(tr("[ERROR] Could not launch the adapter settings page"));
        return;
    }

    Q_EMIT logOutput(tr("Opened adapter settings for '%1'").arg(adapter->name));
    Q_EMIT statusMessage(tr("Opened adapter settings for '%1'").arg(adapter->name),
                         sak::kTimerStatusMessageMs);
}

void NetworkDiagnosticPanel::onViewBluetoothDevices() {
    if (!sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"),
                                       {QStringLiteral("ms-settings:bluetooth")})) {
        Q_EMIT logOutput(tr("[ERROR] Could not launch Bluetooth settings"));
        return;
    }
    Q_EMIT logOutput(tr("Opened Bluetooth devices settings"));
    Q_EMIT statusMessage(tr("Opened Bluetooth devices settings"), sak::kTimerStatusMessageMs);
    sak::logInfo("Opened Bluetooth devices settings");
}

void NetworkDiagnosticPanel::onBridgeConnections() {
    const auto selected = selectedAdapters();
    if (selected.size() < kMinimumBridgeAdapterCount) {
        sak::showInformationLogged(this,
                                   tr("Bridge Connections"),
                                   tr("Select two or more adapters to create a network bridge."));
        return;
    }

    QStringList adapter_names;
    for (const auto* sel : selected) {
        adapter_names << sel->name;
    }

    auto confirm = sak::showQuestionLogged(this,
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

    if (!sak::startDetachedSystem32Tool(QStringLiteral("control.exe"),
                                        {QStringLiteral("ncpa.cpl")})) {
        Q_EMIT logOutput(tr("[ERROR] Could not launch Network Connections"));
        return;
    }
    Q_EMIT logOutput(tr("Bridge requested for: %1").arg(adapter_names.join(QStringLiteral(", "))));
    Q_EMIT statusMessage(
        tr("Opened Network Connections for bridging %1 adapters").arg(selected.size()),
        sak::kTimerStatusDefaultMs);
    sak::logInfo("Bridge connection requested for {} adapters", std::to_string(selected.size()));
}

// -- IP Configuration Actions --------------------------------------------

bool NetworkDiagnosticPanel::promptStaticIpInput(const NetworkAdapterInfo& adapter,
                                                 QString& ip,
                                                 QString& mask,
                                                 QString& gateway) {
    constexpr int kIpDialogWidth = 420;
    const QString ip_pattern = QStringLiteral(
        "^((25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)\\.){3}"
        "(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)$");

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Set Static IP \xe2\x80\x94 %1").arg(adapter.name));
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

    const auto current_ip = adapter.ipv4Addresses.isEmpty() ? QString()
                                                            : adapter.ipv4Addresses.first();
    const auto current_mask = adapter.ipv4SubnetMasks.isEmpty() ? QString()
                                                                : adapter.ipv4SubnetMasks.first();

    auto* ip_edit = make_ip_row(tr("IP Address:"), current_ip, QStringLiteral("192.168.1.100"));
    auto* mask_edit = make_ip_row(tr("Subnet Mask:"),
                                  current_mask.isEmpty() ? QStringLiteral("255.255.255.0")
                                                         : current_mask,
                                  QStringLiteral("255.255.255.0"));
    auto* gw_edit = make_ip_row(tr("Gateway:"), adapter.ipv4Gateway, QStringLiteral("192.168.1.1"));

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            &dialog);
    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(button_box);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    ip = ip_edit->text().trimmed();
    mask = mask_edit->text().trimmed();
    gateway = gw_edit->text().trimmed();
    return true;
}

void NetworkDiagnosticPanel::onSetStaticIp() {
    const NetworkAdapterInfo* selected = selectedAdapter();
    if (selected == nullptr) {
        return;
    }
    // Snapshot the adapter by value before the nested event loop below (dialog or message
    // box): a queued adapter rescan delivered while it runs reassigns m_adapters and frees
    // what selectedAdapter() returned, so a raw pointer would dangle (use-after-free).
    const NetworkAdapterInfo adapter_snapshot = *selected;

    QString ip;
    QString mask;
    QString gateway;
    if (!promptStaticIpInput(adapter_snapshot, ip, mask, gateway)) {
        return;
    }

    if (ip.isEmpty() || mask.isEmpty()) {
        sak::showWarningLogged(this,
                               tr("Invalid Input"),
                               tr("IP address and subnet mask are required."));
        return;
    }

    applyStaticIp(adapter_snapshot.name, ip, mask, gateway);
}

void NetworkDiagnosticPanel::applyStaticIp(const QString& adapter_name,
                                           const QString& ip,
                                           const QString& mask,
                                           const QString& gateway) {
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

    runNetshCommandAsync(
        args, [this, adapter_name, ip, mask, gateway](bool success, const QString& output) {
            if (success) {
                Q_EMIT statusMessage(tr("Static IP configured on '%1'").arg(adapter_name),
                                     sak::kTimerStatusMessageMs);
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
                sak::showWarningLogged(this,
                                       tr("Failed to Set IP"),
                                       tr("Failed to configure static IP.\n\n"
                                          "Administrator privileges may be required.\n\n%1")
                                           .arg(output));
            }
        });
}

void NetworkDiagnosticPanel::onSetDnsServers() {
    const NetworkAdapterInfo* selected = selectedAdapter();
    if (selected == nullptr) {
        return;
    }
    // Snapshot the adapter by value before the nested event loop below (dialog or message
    // box): a queued adapter rescan delivered while it runs reassigns m_adapters and frees
    // what selectedAdapter() returned, so a raw pointer would dangle (use-after-free).
    const NetworkAdapterInfo adapter_snapshot = *selected;
    const NetworkAdapterInfo* adapter = &adapter_snapshot;

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
        sak::showWarningLogged(this, tr("Invalid Input"), tr("Primary DNS server is required."));
        return;
    }

    applyDnsServers(adapter->name, primary, secondary);
}

namespace {

using TrackedFuture = QFuture<QPair<bool, QString>>;

// Output text of a failed netsh step (timeout message or captured stderr/stdout).
QString netshStepOutput(const ProcessResult& process) {
    if (process.timed_out) {
        return NetworkDiagnosticPanel::tr("netsh command timed out");
    }
    return process.std_out.isEmpty() ? process.std_err : process.std_out;
}

// Runs netsh steps in order on a worker thread, stopping at the first failure.
// Returns {index of first failed step (or -1 if all succeeded), its output}.
QPair<int, QString> runNetshStepsSequential(const QList<QStringList>& steps, int timeout_ms) {
    // System32-qualified netsh only; unresolvable means step 0 failed (fail closed), never
    // a PATH/CWD-resolved launch of a privileged DNS/adapter mutation.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        return {0, NetworkDiagnosticPanel::tr("Cannot resolve the System32 netsh.exe path")};
    }
    for (int i = 0; i < steps.size(); ++i) {
        const auto process = runProcess(netsh_exe, steps.at(i), timeout_ms);
        if (!process.succeeded()) {
            return {i, netshStepOutput(process)};
        }
    }
    return {-1, QString()};
}

// Maps a DNS-apply outcome to an honest {success, message}. A failed secondary
// step is reported as a partial failure, never as success (fail closed).
QPair<bool, QString> dnsApplyMessage(const QString& adapter,
                                     const QString& primary,
                                     const QString& secondary,
                                     int failed_index,
                                     const QString& output) {
    if (failed_index < 0) {
        return {true, NetworkDiagnosticPanel::tr("DNS servers configured on '%1'").arg(adapter)};
    }
    if (failed_index == 0) {
        return {false,
                NetworkDiagnosticPanel::tr("Failed to set primary DNS on '%1'. Administrator "
                                           "privileges may be required.\n\n%2")
                    .arg(adapter, output)};
    }
    return {false,
            NetworkDiagnosticPanel::tr("Partially configured DNS on '%1': primary %2 was set, "
                                       "but adding secondary %3 failed.\n\n%4")
                .arg(adapter, primary, secondary, output)};
}

// Applies primary (and optional secondary) DNS as ONE joined worker unit so the
// sequence cannot be truncated mid-way by panel teardown.
QPair<bool, QString> runDnsApplySequence(const QString& adapter,
                                         const QString& primary,
                                         const QString& secondary,
                                         int timeout_ms) {
    QList<QStringList> steps;
    steps.append({QStringLiteral("interface"),
                  QStringLiteral("ipv4"),
                  QStringLiteral("set"),
                  QStringLiteral("dns"),
                  adapter,
                  QStringLiteral("static"),
                  primary});
    if (!secondary.isEmpty()) {
        steps.append({QStringLiteral("interface"),
                      QStringLiteral("ipv4"),
                      QStringLiteral("add"),
                      QStringLiteral("dns"),
                      adapter,
                      secondary,
                      QStringLiteral("index=2")});
    }
    const auto [failed_index, output] = runNetshStepsSequential(steps, timeout_ms);
    return dnsApplyMessage(adapter, primary, secondary, failed_index, output);
}

// Maps a DHCP-enable outcome to an honest {success, message}. A failed
// DNS-to-DHCP step is reported as a partial failure, never as success.
QPair<bool, QString> dhcpApplyMessage(const QString& adapter,
                                      int failed_index,
                                      const QString& output) {
    if (failed_index < 0) {
        return {true, NetworkDiagnosticPanel::tr("DHCP enabled on '%1'").arg(adapter)};
    }
    if (failed_index == 0) {
        return {false,
                NetworkDiagnosticPanel::tr("Failed to enable DHCP on '%1'. Administrator "
                                           "privileges may be required.\n\n%2")
                    .arg(adapter, output)};
    }
    return {false,
            NetworkDiagnosticPanel::tr("Partially enabled DHCP on '%1': the IP address was "
                                       "switched to DHCP, but resetting DNS to DHCP failed.\n\n%2")
                .arg(adapter, output)};
}

// Switches the adapter's IP and DNS to DHCP as ONE joined worker unit.
QPair<bool, QString> runDhcpEnableSequence(const QString& adapter, int timeout_ms) {
    QList<QStringList> steps;
    steps.append({QStringLiteral("interface"),
                  QStringLiteral("ipv4"),
                  QStringLiteral("set"),
                  QStringLiteral("address"),
                  adapter,
                  QStringLiteral("dhcp")});
    steps.append({QStringLiteral("interface"),
                  QStringLiteral("ipv4"),
                  QStringLiteral("set"),
                  QStringLiteral("dns"),
                  adapter,
                  QStringLiteral("dhcp")});
    const auto [failed_index, output] = runNetshStepsSequential(steps, timeout_ms);
    return dhcpApplyMessage(adapter, failed_index, output);
}

// Launches a multi-step netsh mutation as a single joined worker unit and tracks
// its future so panel teardown can bounded-wait the WHOLE sequence rather than
// truncate it. The report callback is context-bound to `owner`, so no slot fires
// on a destroyed panel.
void launchTrackedNetshSequence(NetworkDiagnosticPanel* owner,
                                QList<TrackedFuture>& tracked,
                                std::function<QPair<bool, QString>()> work,
                                std::function<void(bool, const QString&)> report) {
    auto* watcher = new QFutureWatcher<QPair<bool, QString>>(owner);
    QObject::connect(watcher,
                     &QFutureWatcher<QPair<bool, QString>>::finished,
                     owner,
                     [watcher, report = std::move(report)]() {
                         const auto [success, message] = watcher->result();
                         watcher->deleteLater();
                         report(success, message);
                     });
    const TrackedFuture future = QtConcurrent::run(std::move(work));
    watcher->setFuture(future);
    tracked.removeIf([](const TrackedFuture& f) { return f.isFinished(); });
    tracked.append(future);
}

}  // namespace

void NetworkDiagnosticPanel::applyDnsServers(const QString& adapter_name,
                                             const QString& primary,
                                             const QString& secondary) {
    Q_EMIT logOutput(
        tr("Setting DNS on '%1': primary=%2 secondary=%3").arg(adapter_name, primary, secondary));

    const int timeout = sak::kTimeoutNetworkReadMs;
    launchTrackedNetshSequence(
        this,
        m_pending_command_futures,
        [adapter_name, primary, secondary]() {
            return runDnsApplySequence(adapter_name, primary, secondary, timeout);
        },
        [this, adapter_name, primary, secondary](bool success, const QString& message) {
            if (success) {
                Q_EMIT statusMessage(message, sak::kTimerStatusMessageMs);
                Q_EMIT logOutput(message);
                sak::logInfo("DNS set on {}: primary={} secondary={}",
                             adapter_name.toStdString(),
                             primary.toStdString(),
                             secondary.toStdString());
            } else {
                sak::logError("DNS apply failed on {}: {}",
                              adapter_name.toStdString(),
                              message.toStdString());
                Q_EMIT logOutput(tr("[ERROR] %1").arg(message));
                sak::showWarningLogged(this, tr("DNS Configuration Failed"), message);
            }
            onRefreshAdapters();
        });
}

void NetworkDiagnosticPanel::onEnableDhcp() {
    const NetworkAdapterInfo* selected = selectedAdapter();
    if (selected == nullptr) {
        return;
    }
    // Snapshot the adapter by value before the nested event loop below (dialog or message
    // box): a queued adapter rescan delivered while it runs reassigns m_adapters and frees
    // what selectedAdapter() returned, so a raw pointer would dangle (use-after-free).
    const NetworkAdapterInfo adapter_snapshot = *selected;
    const NetworkAdapterInfo* adapter = &adapter_snapshot;

    auto confirm =
        sak::showQuestionLogged(this,
                                tr("Enable DHCP"),
                                tr("Switch adapter <b>%1</b> to DHCP?\n\n"
                                   "The current static IP configuration will be removed.")
                                    .arg(adapter->name.toHtmlEscaped()),
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    Q_EMIT logOutput(tr("Enabling DHCP on '%1'...").arg(adapter->name));

    const QString adapter_name = adapter->name;
    const int timeout = sak::kTimeoutNetworkReadMs;
    launchTrackedNetshSequence(
        this,
        m_pending_command_futures,
        [adapter_name]() { return runDhcpEnableSequence(adapter_name, timeout); },
        [this, adapter_name](bool success, const QString& message) {
            if (success) {
                Q_EMIT statusMessage(message, sak::kTimerStatusMessageMs);
                Q_EMIT logOutput(message);
                sak::logInfo("DHCP enabled on: {}", adapter_name.toStdString());
            } else {
                sak::logError("DHCP enable failed on {}: {}",
                              adapter_name.toStdString(),
                              message.toStdString());
                Q_EMIT logOutput(tr("[ERROR] %1").arg(message));
                sak::showWarningLogged(this, tr("DHCP Failed"), message);
            }
            onRefreshAdapters();
        });
}

void NetworkDiagnosticPanel::onReleaseDhcpLease() {
    const auto* adapter = selectedAdapter();
    if (adapter == nullptr) {
        return;
    }

    Q_EMIT logOutput(tr("Releasing DHCP lease on '%1'...").arg(adapter->name));

    constexpr int kIpconfigTimeoutMs = 10'000;
    const QString adapter_name = adapter->name;
    // System32-qualified ipconfig, never the bare name (same hijack surface as netsh).
    const QString ipconfig_exe = sak::system32Path(QStringLiteral("ipconfig.exe"));
    if (ipconfig_exe.isEmpty()) {
        Q_EMIT logOutput(tr("[ERROR] Cannot resolve the System32 ipconfig.exe path"));
        return;
    }
    runCommandAsync(
        ipconfig_exe,
        {QStringLiteral("/release"), adapter_name},
        kIpconfigTimeoutMs,
        [this, adapter_name](bool success, const QString& output) {
            if (success) {
                Q_EMIT statusMessage(tr("DHCP lease released on '%1'").arg(adapter_name),
                                     sak::kTimerStatusMessageMs);
                Q_EMIT logOutput(tr("DHCP lease released on '%1'").arg(adapter_name));
                sak::logInfo("DHCP lease released: {}", adapter_name.toStdString());
            } else {
                sak::logWarning("DHCP release may have failed: {}", output.toStdString());
                Q_EMIT logOutput(
                    tr("[WARN] DHCP release may have failed on '%1'").arg(adapter_name));
            }

            onRefreshAdapters();
        });
}

void NetworkDiagnosticPanel::onRenewDhcpLease() {
    const auto* adapter = selectedAdapter();
    if (adapter == nullptr) {
        return;
    }

    Q_EMIT statusMessage(tr("Renewing DHCP lease on '%1'...").arg(adapter->name), 0);
    Q_EMIT logOutput(tr("Renewing DHCP lease on '%1'...").arg(adapter->name));

    constexpr int kIpconfigTimeoutMs = 30'000;
    const QString adapter_name = adapter->name;
    // System32-qualified ipconfig, never the bare name (same hijack surface as netsh).
    const QString ipconfig_exe = sak::system32Path(QStringLiteral("ipconfig.exe"));
    if (ipconfig_exe.isEmpty()) {
        Q_EMIT logOutput(tr("[ERROR] Cannot resolve the System32 ipconfig.exe path"));
        return;
    }
    runCommandAsync(ipconfig_exe,
                    {QStringLiteral("/renew"), adapter_name},
                    kIpconfigTimeoutMs,
                    [this, adapter_name](bool success, const QString& output) {
                        if (success) {
                            Q_EMIT statusMessage(tr("DHCP lease renewed on '%1'").arg(adapter_name),
                                                 sak::kTimerStatusMessageMs);
                            Q_EMIT logOutput(tr("DHCP lease renewed on '%1'").arg(adapter_name));
                            sak::logInfo("DHCP lease renewed: {}", adapter_name.toStdString());
                        } else {
                            sak::logWarning("DHCP renew may have failed: {}", output.toStdString());
                            Q_EMIT logOutput(
                                tr("[WARN] DHCP renew may have failed on '%1'").arg(adapter_name));
                        }

                        onRefreshAdapters();
                    });
}

// -- Ping --

void NetworkDiagnosticPanel::onStartPing() {
    Q_ASSERT(m_pingTarget);
    Q_ASSERT(m_pingTable);
    const auto target = m_pingTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a ping target"), sak::kTimerStatusMessageMs);
        return;
    }

    m_pingTable->setRowCount(0);
    m_pingStatsLabel->clear();
    m_pingStartBtn->setEnabled(false);
    m_pingStopBtn->setEnabled(true);
    m_pingEmptyState->setLoading(tr("Pinging target host..."));

    m_controller->ping({.target = target,
                        .count = m_pingCount->value(),
                        .interval_ms = m_pingInterval->value(),
                        .timeout_ms = m_pingTimeout->value(),
                        .packet_size = m_pingPacketSize->value(),
                        .ttl = kDefaultPingTtl});
}

void NetworkDiagnosticPanel::onStopPing() {
    Q_ASSERT(m_controller);
    m_controller->cancel();
    m_pingStartBtn->setEnabled(true);
    m_pingStopBtn->setEnabled(false);
    m_pingEmptyState->clearLoading();
}

void NetworkDiagnosticPanel::onPingReply(PingReply reply) {
    Q_ASSERT(m_pingTable);
    // Lift the loading overlay as soon as replies stream in so it never masks data.
    m_pingEmptyState->clearLoading();
    const int row = m_pingTable->rowCount();
    m_pingTable->insertRow(row);

    m_pingTable->setItem(row,
                         KPingColumnSequence,
                         new QTableWidgetItem(QString::number(reply.sequenceNumber)));
    m_pingTable->setItem(row, KPingColumnAddress, new QTableWidgetItem(reply.replyFrom));

    auto* status_item = new QTableWidgetItem(reply.success ? tr("Reply") : tr("Timeout"));
    status_item->setForeground(reply.success ? QColor(ui::kColorSuccess) : QColor(ui::kColorError));
    m_pingTable->setItem(row, KPingColumnStatus, status_item);

    m_pingTable->setItem(
        row,
        KPingColumnRtt,
        new QTableWidgetItem(
            reply.success ? QStringLiteral("%1").arg(reply.rttMs, 0, 'f', kDecimalPrecisionOne)
                          : QStringLiteral("--")));
    m_pingTable->setItem(row,
                         KPingColumnTtl,
                         new QTableWidgetItem(reply.success ? QString::number(reply.ttl)
                                                            : QStringLiteral("--")));

    m_pingTable->scrollToBottom();
}

void NetworkDiagnosticPanel::onPingComplete(PingResult result) {
    Q_ASSERT(m_pingStartBtn);
    Q_ASSERT(m_pingStopBtn);
    m_pingStartBtn->setEnabled(true);
    m_pingStopBtn->setEnabled(false);
    m_pingEmptyState->clearLoading();

    m_pingStatsLabel->setText(QStringLiteral("Sent: %1 | Rcvd: %2 | Lost: %3 (%4%) | "
                                             "Min: %5 ms | Max: %6 ms | Avg: %7 ms | Jitter: %8 ms")
                                  .arg(result.sent)
                                  .arg(result.received)
                                  .arg(result.lost)
                                  .arg(result.lossPercent, 0, 'f', kDecimalPrecisionOne)
                                  .arg(result.minRtt, 0, 'f', kDecimalPrecisionOne)
                                  .arg(result.maxRtt, 0, 'f', kDecimalPrecisionOne)
                                  .arg(result.avgRtt, 0, 'f', kDecimalPrecisionOne)
                                  .arg(result.jitter, 0, 'f', kDecimalPrecisionTwo));

    Q_EMIT statusMessage(QStringLiteral("Ping complete -- %1% loss")
                             .arg(result.lossPercent, 0, 'f', kDecimalPrecisionOne),
                         sak::kTimerStatusDefaultMs);
}

// -- Traceroute --

void NetworkDiagnosticPanel::onStartTraceroute() {
    Q_ASSERT(m_traceTarget);
    Q_ASSERT(m_traceTable);
    const auto target = m_traceTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a traceroute target"), sak::kTimerStatusMessageMs);
        return;
    }

    m_traceTable->setRowCount(0);
    m_traceStatusLabel->clear();
    m_traceStartBtn->setEnabled(false);
    m_traceStopBtn->setEnabled(true);
    m_traceEmptyState->setLoading(tr("Tracing the network route..."));

    m_controller->traceroute(
        target, m_traceMaxHops->value(), kTraceDefaultTimeoutMs, kTraceDefaultProbeCount, true);
}

void NetworkDiagnosticPanel::onStopTraceroute() {
    m_controller->cancel();
    m_traceStartBtn->setEnabled(true);
    m_traceStopBtn->setEnabled(false);
    m_traceEmptyState->clearLoading();
}

void NetworkDiagnosticPanel::onTracerouteHop(TracerouteHop hop) {
    Q_ASSERT(m_traceTable);
    Q_ASSERT(m_traceStatusLabel);
    // Lift the loading overlay as soon as hops stream in so it never masks data.
    m_traceEmptyState->clearLoading();
    const int row = m_traceTable->rowCount();
    m_traceTable->insertRow(row);

    m_traceTable->setItem(row,
                          KTraceColumnHop,
                          new QTableWidgetItem(QString::number(hop.hopNumber)));

    if (hop.timedOut) {
        auto* timeout_item = new QTableWidgetItem(tr("* * * Request timed out"));
        timeout_item->setForeground(QColor(ui::kColorWarning));
        m_traceTable->setItem(row, KTraceColumnAddress, timeout_item);
        for (int i = KTraceColumnHostname; i < KTraceColumnCount; ++i) {
            m_traceTable->setItem(row, i, new QTableWidgetItem(QStringLiteral("*")));
        }
    } else {
        m_traceTable->setItem(row, KTraceColumnAddress, new QTableWidgetItem(hop.ipAddress));
        m_traceTable->setItem(row,
                              KTraceColumnHostname,
                              new QTableWidgetItem(hop.hostname.isEmpty() ? QStringLiteral("--")
                                                                          : hop.hostname));
        m_traceTable->setItem(row,
                              KTraceColumnRttFirst,
                              new QTableWidgetItem(QStringLiteral("%1").arg(
                                  hop.rtt1Ms, 0, 'f', kDecimalPrecisionOne)));
        m_traceTable->setItem(row,
                              KTraceColumnRttSecond,
                              new QTableWidgetItem(QStringLiteral("%1").arg(
                                  hop.rtt2Ms, 0, 'f', kDecimalPrecisionOne)));
        m_traceTable->setItem(row,
                              KTraceColumnRttThird,
                              new QTableWidgetItem(QStringLiteral("%1").arg(
                                  hop.rtt3Ms, 0, 'f', kDecimalPrecisionOne)));
        m_traceTable->setItem(row,
                              KTraceColumnAverage,
                              new QTableWidgetItem(QStringLiteral("%1").arg(
                                  hop.avgRttMs, 0, 'f', kDecimalPrecisionOne)));
    }

    m_traceStatusLabel->setText(QStringLiteral("Hop %1...").arg(hop.hopNumber));
    m_traceTable->scrollToBottom();
}

void NetworkDiagnosticPanel::onTracerouteComplete(TracerouteResult result) {
    Q_ASSERT(m_traceStartBtn);
    Q_ASSERT(m_traceStopBtn);
    m_traceStartBtn->setEnabled(true);
    m_traceStopBtn->setEnabled(false);
    m_traceEmptyState->clearLoading();

    QString status;
    if (result.reachedTarget) {
        status = QStringLiteral("Reached %1 in %2 hops").arg(result.target).arg(result.hops.size());
    } else {
        status = QStringLiteral("Could not reach %1 (%2 hops)")
                     .arg(result.target)
                     .arg(result.hops.size());
    }
    m_traceStatusLabel->setText(status);
    Q_EMIT statusMessage(status, sak::kTimerStatusDefaultMs);
}

// -- MTR --

void NetworkDiagnosticPanel::onStartMtr() {
    Q_ASSERT(m_mtrTarget);
    Q_ASSERT(m_mtrTable);
    const auto target = m_mtrTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter an MTR target"), sak::kTimerStatusMessageMs);
        return;
    }

    m_mtrTable->setRowCount(0);
    m_mtrStatusLabel->clear();
    m_mtrStartBtn->setEnabled(false);
    m_mtrStopBtn->setEnabled(true);
    m_mtrEmptyState->setLoading(tr("Running MTR analysis..."));

    m_controller->mtr(target,
                      m_mtrCycles->value(),
                      kMtrDefaultIntervalMs,
                      kTraceDefaultHops,
                      kMtrDefaultTimeoutMs);
}

void NetworkDiagnosticPanel::onStopMtr() {
    m_controller->cancel();
    m_mtrStartBtn->setEnabled(true);
    m_mtrStopBtn->setEnabled(false);
    m_mtrEmptyState->clearLoading();
}

void NetworkDiagnosticPanel::onMtrUpdate(QVector<MtrHopStats> hops, int cycle) {
    Q_ASSERT(m_mtrTable);
    Q_ASSERT(m_mtrStatusLabel);
    // Lift the loading overlay as soon as the first cycle arrives so data is visible.
    m_mtrEmptyState->clearLoading();
    m_mtrTable->setSortingEnabled(false);
    m_mtrTable->setRowCount(static_cast<int>(hops.size()));

    for (int i = 0; i < hops.size(); ++i) {
        const auto& h = hops[i];

        // Reuse existing items to avoid allocation churn on frequent updates
        auto set_or_create = [this](int row, int col, const QString& text) {
            if (auto* existing = m_mtrTable->item(row, col)) {
                existing->setText(text);
            } else {
                m_mtrTable->setItem(row, col, new QTableWidgetItem(text));
            }
        };

        set_or_create(i, KMtrColumnHop, QString::number(h.hopNumber));

        const auto label = h.hostname.isEmpty()
                               ? h.ipAddress
                               : QStringLiteral("%1 (%2)").arg(h.hostname, h.ipAddress);
        set_or_create(i, KMtrColumnAddress, label);

        const auto loss_text =
            QStringLiteral("%1").arg(h.lossPercent, 0, 'f', kDecimalPrecisionOne);
        if (auto* existing = m_mtrTable->item(i, KMtrColumnLoss)) {
            existing->setText(loss_text);
            if (h.lossPercent > kMtrLossWarningPercentF) {
                existing->setForeground(QColor(ui::kColorError));
            } else if (h.lossPercent > 0.0) {
                existing->setForeground(QColor(ui::kColorWarning));
            } else {
                existing->setForeground(QColor());
            }
        } else {
            auto* loss_item = new QTableWidgetItem(loss_text);
            if (h.lossPercent > kMtrLossWarningPercentF) {
                loss_item->setForeground(QColor(ui::kColorError));
            } else if (h.lossPercent > 0.0) {
                loss_item->setForeground(QColor(ui::kColorWarning));
            }
            m_mtrTable->setItem(i, KMtrColumnLoss, loss_item);
        }

        set_or_create(i, KMtrColumnSent, QString::number(h.sent));
        set_or_create(i,
                      KMtrColumnAverage,
                      QStringLiteral("%1").arg(h.avgRttMs, 0, 'f', kDecimalPrecisionOne));
        set_or_create(i,
                      KMtrColumnBest,
                      QStringLiteral("%1").arg(h.bestRttMs, 0, 'f', kDecimalPrecisionOne));
        set_or_create(i,
                      KMtrColumnWorst,
                      QStringLiteral("%1").arg(h.worstRttMs, 0, 'f', kDecimalPrecisionOne));
        set_or_create(i,
                      KMtrColumnJitter,
                      QStringLiteral("%1").arg(h.jitterMs, 0, 'f', kDecimalPrecisionTwo));
    }

    m_mtrTable->setSortingEnabled(true);
    m_mtrStatusLabel->setText(QStringLiteral("Cycle %1/%2").arg(cycle).arg(m_mtrCycles->value()));
}

void NetworkDiagnosticPanel::onMtrComplete(MtrResult result) {
    Q_ASSERT(m_mtrStartBtn);
    Q_ASSERT(m_mtrStopBtn);
    m_mtrStartBtn->setEnabled(true);
    m_mtrStopBtn->setEnabled(false);
    m_mtrEmptyState->clearLoading();

    const int hops = static_cast<int>(result.hops.size());
    const QString status = QStringLiteral("MTR complete -- %1 hops, %2 cycles to %3")
                               .arg(hops)
                               .arg(result.totalCycles)
                               .arg(result.target);
    m_mtrStatusLabel->setText(status);
    Q_EMIT logOutput(status);
    Q_EMIT statusMessage(status, sak::kTimerStatusDefaultMs);
}

// -- DNS --

void NetworkDiagnosticPanel::onDnsQuery() {
    Q_ASSERT(m_dnsHostname);
    Q_ASSERT(m_dnsQueryBtn);
    const auto hostname = m_dnsHostname->text().trimmed();
    if (hostname.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a hostname"), sak::kTimerStatusMessageMs);
        return;
    }

    m_dnsQueryBtn->setEnabled(false);
    m_dnsStatusLabel->setText(tr("Querying..."));
    m_dnsEmptyState->setLoading(tr("Resolving DNS query..."));
    // Use currentData if a known entry is selected; otherwise parse
    // the user-typed text for a manually-entered DNS server IP.
    const auto server_data = m_dnsServer->currentData();
    const auto server = server_data.isValid() ? server_data.toString()
                                              : m_dnsServer->currentText().trimmed();
    m_controller->dnsQuery(hostname, m_dnsRecordType->currentText(), server);
}

void NetworkDiagnosticPanel::onDnsReverseLookup() {
    Q_ASSERT(m_dnsHostname);
    Q_ASSERT(m_dnsReverseBtn);
    const auto ip = m_dnsHostname->text().trimmed();
    if (ip.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter an IP address"), sak::kTimerStatusMessageMs);
        return;
    }

    m_dnsReverseBtn->setEnabled(false);
    m_dnsStatusLabel->setText(tr("Resolving..."));
    m_dnsEmptyState->setLoading(tr("Resolving DNS query..."));
    const auto server_data = m_dnsServer->currentData();
    const auto server = server_data.isValid() ? server_data.toString()
                                              : m_dnsServer->currentText().trimmed();
    m_controller->dnsReverseLookup(ip, server);
}

void NetworkDiagnosticPanel::onDnsCompare() {
    Q_ASSERT(m_dnsHostname);
    Q_ASSERT(m_dnsServer);
    const auto hostname = m_dnsHostname->text().trimmed();
    if (hostname.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a hostname"), sak::kTimerStatusMessageMs);
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
    m_dnsEmptyState->setLoading(tr("Resolving DNS query..."));
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
    m_dnsEmptyState->clearLoading();

    const int row = m_dnsTable->rowCount();
    m_dnsTable->insertRow(row);

    m_dnsTable->setItem(row, KDnsColumnQuery, new QTableWidgetItem(result.queryName));
    m_dnsTable->setItem(row, KDnsColumnType, new QTableWidgetItem(result.recordType));
    m_dnsTable->setItem(row, KDnsColumnServer, new QTableWidgetItem(result.dnsServer));
    m_dnsTable->setItem(row,
                        KDnsColumnResponseTime,
                        new QTableWidgetItem(QStringLiteral("%1 ms").arg(
                            result.responseTimeMs, 0, 'f', kDecimalPrecisionOne)));

    auto* answer_item = new QTableWidgetItem(result.answers.join(QStringLiteral(", ")));
    if (!result.success) {
        answer_item->setText(result.errorMessage);
        answer_item->setForeground(QColor(ui::kColorError));
    }
    m_dnsTable->setItem(row, KDnsColumnAnswers, answer_item);

    if (result.success) {
        m_dnsStatusLabel->setText(QStringLiteral("%1 answers in %2 ms")
                                      .arg(result.answers.size())
                                      .arg(result.responseTimeMs, 0, 'f', kDecimalPrecisionOne));
    } else {
        m_dnsStatusLabel->setText(QStringLiteral("Query failed: %1").arg(result.errorMessage));
    }
}

void NetworkDiagnosticPanel::onDnsComparisonComplete(DnsServerComparison comparison) {
    Q_ASSERT(m_dnsCompareBtn);
    Q_ASSERT(m_dnsTable);
    m_dnsCompareBtn->setEnabled(true);
    m_dnsEmptyState->clearLoading();

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
    bool has_primary = false;
};

QVector<uint16_t> getPresetPorts(int preset_idx) {
    if (preset_idx <= 0) {
        return {};
    }

    const auto presets = sak::PortScanner::getPresets();
    const int index = preset_idx - 1;
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
    if (!range.has_primary) {
        range.start = start;
        range.end = end;
        range.has_primary = true;
        return;
    }
    appendPortRange(ports, start, end);
}

constexpr unsigned int kMaxPortValue = 65'535;

bool parsePortRange(const QString& text, QVector<uint16_t>& ports, PortScanRange& range) {
    const auto range_parts = text.split('-');
    if (range_parts.size() != kPortRangePartCount) {
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

QVector<uint16_t> parseCustomPorts(const QString& custom_text, PortScanRange& range) {
    if (custom_text.isEmpty()) {
        static const QVector<uint16_t> kDefaultCustomScanPorts = {
            21, 22, 23, 25, 53, 80, 110, 143, 443, 445, 993, 995, 3306, 3389, 5432, 8080, 8443};
        return kDefaultCustomScanPorts;
    }

    QVector<uint16_t> ports;
    const auto parts = custom_text.split(',', Qt::SkipEmptyParts);
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

    const auto all_presets = PortScanner::getPresets();
    const int preset_index = index - 1;
    if (preset_index < 0 || preset_index >= all_presets.size()) {
        m_portCustomRange->setEnabled(true);
        m_portCustomRange->clear();
        return;
    }

    const auto& preset = all_presets[preset_index];
    QStringList port_strs;
    port_strs.reserve(preset.ports.size());
    for (auto port : preset.ports) {
        port_strs.append(QString::number(port));
    }

    m_portCustomRange->setText(port_strs.join(QStringLiteral(",")));
    m_portCustomRange->setEnabled(false);
}

void NetworkDiagnosticPanel::onStartPortScan() {
    Q_ASSERT(m_portTarget);
    Q_ASSERT(m_portTable);
    const auto target = m_portTarget->text().trimmed();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter a port scan target"), sak::kTimerStatusMessageMs);
        return;
    }

    m_portTable->setRowCount(0);
    m_portSummaryLabel->clear();
    m_portProgress->setValue(0);
    m_portProgress->setVisible(true);
    m_portStartBtn->setEnabled(false);
    m_portStopBtn->setEnabled(true);
    m_portEmptyState->setLoading(tr("Scanning target ports..."));

    PortScanRange range;
    QVector<uint16_t> ports;
    const int preset_idx = m_portPreset->currentIndex();
    if (preset_idx > 0) {
        ports = getPresetPorts(preset_idx);
    } else {
        ports = parseCustomPorts(m_portCustomRange->text().trimmed(), range);
    }

    m_controller->scanPorts({.target = target,
                             .ports = ports,
                             .range_start = range.start,
                             .range_end = range.end,
                             .timeout_ms = m_portTimeout->value(),
                             .max_concurrent = m_portConcurrent->value(),
                             .grab_banners = m_portBannerGrab->isChecked()});
}

void NetworkDiagnosticPanel::onStopPortScan() {
    Q_ASSERT(m_controller);
    m_controller->cancel();
    m_portStartBtn->setEnabled(true);
    m_portStopBtn->setEnabled(false);
    m_portProgress->setVisible(false);
    m_portEmptyState->clearLoading();
}

void NetworkDiagnosticPanel::onPortScanned(PortScanResult result) {
    Q_ASSERT(m_portTable);
    // Only show open ports in real-time to avoid table bloat
    if (result.state != PortScanResult::State::Open) {
        return;
    }
    // First open port arrived: lift the loading overlay so it never masks results.
    m_portEmptyState->clearLoading();

    m_portTable->setSortingEnabled(false);
    const int row = m_portTable->rowCount();
    m_portTable->insertRow(row);

    m_portTable->setItem(row, KPortColumnPort, new QTableWidgetItem(QString::number(result.port)));

    auto* state_item = new QTableWidgetItem(tr("Open"));
    state_item->setForeground(QColor(ui::kColorSuccess));
    m_portTable->setItem(row, KPortColumnState, state_item);

    m_portTable->setItem(row, KPortColumnService, new QTableWidgetItem(result.serviceName));
    m_portTable->setItem(row,
                         KPortColumnResponse,
                         new QTableWidgetItem(QStringLiteral("%1").arg(
                             result.responseTimeMs, 0, 'f', kDecimalPrecisionOne)));
    m_portTable->setItem(row,
                         KPortColumnBanner,
                         new QTableWidgetItem(result.banner.left(kPortBannerPreviewChars)));

    m_portTable->setSortingEnabled(true);
    m_portTable->scrollToBottom();
}

void NetworkDiagnosticPanel::onPortScanProgress(int scanned, int total) {
    if (total > 0) {
        m_portProgress->setValue(static_cast<int>(static_cast<double>(scanned) / total *
                                                  static_cast<double>(progress::kComplete)));
    }
    m_portSummaryLabel->setText(QStringLiteral("%1/%2 scanned").arg(scanned).arg(total));
}

void NetworkDiagnosticPanel::onPortScanComplete(QVector<PortScanResult> results) {
    Q_ASSERT(m_portStartBtn);
    Q_ASSERT(m_portStopBtn);
    m_portStartBtn->setEnabled(true);
    m_portStopBtn->setEnabled(false);
    m_portProgress->setVisible(false);
    m_portEmptyState->clearLoading();

    int open_count = 0;
    int closed_count = 0;
    int filtered_count = 0;
    for (const auto& r : results) {
        switch (r.state) {
        case PortScanResult::State::Open:
            ++open_count;
            break;
        case PortScanResult::State::Closed:
            ++closed_count;
            break;
        case PortScanResult::State::Filtered:
            ++filtered_count;
            break;
        default:
            break;
        }
    }

    m_portSummaryLabel->setText(QStringLiteral("Complete: %1 open, %2 closed, %3 filtered")
                                    .arg(open_count)
                                    .arg(closed_count)
                                    .arg(filtered_count));
}

// -- Bandwidth --

void NetworkDiagnosticPanel::onStartBandwidthTest() {
    Q_ASSERT(m_bwServerAddr);
    Q_ASSERT(m_bwResultLabel);
    const auto server = m_bwServerAddr->text().trimmed();
    if (server.isEmpty()) {
        Q_EMIT statusMessage(tr("Please enter an iPerf3 server address"),
                             sak::kTimerStatusMessageMs);
        return;
    }

    m_bwResultLabel->setText(tr("Running bandwidth test..."));
    m_bwTestBtn->setEnabled(false);

    m_controller->runBandwidthTest({.server_addr = server,
                                    .port = static_cast<uint16_t>(m_bwPort->value()),
                                    .duration_sec = m_bwDuration->value(),
                                    .streams = m_bwStreams->value(),
                                    .bidirectional = m_bwBidirectional->isChecked(),
                                    .udp = false});  // TCP mode
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
                                 .arg(result.downloadMbps, 0, 'f', kDecimalPrecisionTwo)
                                 .arg(result.uploadMbps, 0, 'f', kDecimalPrecisionTwo)
                                 .arg(result.jitterMs, 0, 'f', kDecimalPrecisionTwo)
                                 .arg(result.packetLossPercent, 0, 'f', kDecimalPrecisionTwo)
                                 .arg(result.retransmissions));
}

void NetworkDiagnosticPanel::onHttpSpeedComplete(double down, double up, double latency) {
    m_httpSpeedBtn->setEnabled(true);

    m_httpSpeedLabel->setText(QStringLiteral("<b>Download:</b> %1 Mbps | <b>Upload:</b> %2 Mbps | "
                                             "<b>Latency:</b> %3 ms")
                                  .arg(down, 0, 'f', kDecimalPrecisionTwo)
                                  .arg(up, 0, 'f', kDecimalPrecisionTwo)
                                  .arg(latency, 0, 'f', kDecimalPrecisionOne));
}

// -- WiFi --

void NetworkDiagnosticPanel::onScanWiFi() {
    if (!m_controller->isWiFiAvailable()) {
        Q_EMIT statusMessage(tr("WiFi hardware not available"), sak::kTimerStatusMessageMs);
        return;
    }
    m_wifiScanBtn->setEnabled(false);
    m_wifiEmptyState->setLoading(tr("Scanning for WiFi networks..."));
    m_controller->scanWiFi();
}

void NetworkDiagnosticPanel::onStartContinuousWiFi() {
    if (!m_controller->isWiFiAvailable()) {
        Q_EMIT statusMessage(tr("WiFi hardware not available"), sak::kTimerStatusMessageMs);
        return;
    }
    m_wifiContBtn->setEnabled(false);
    m_wifiStopBtn->setEnabled(true);
    m_wifiEmptyState->setLoading(tr("Scanning for WiFi networks..."));
    m_controller->startContinuousWiFiScan(sak::kTimerRefreshMs);
}

void NetworkDiagnosticPanel::onStopContinuousWiFi() {
    m_controller->stopContinuousWiFiScan();
    m_wifiContBtn->setEnabled(true);
    m_wifiStopBtn->setEnabled(false);
    m_wifiEmptyState->clearLoading();
}

void NetworkDiagnosticPanel::onWiFiScanComplete(QVector<WiFiNetworkInfo> networks) {
    Q_ASSERT(m_wifiScanBtn);
    Q_ASSERT(m_wifiTable);
    m_wifiScanBtn->setEnabled(true);
    m_wifiEmptyState->clearLoading();
    m_wifiTable->setSortingEnabled(false);
    m_wifiTable->setRowCount(0);

    for (const auto& net : networks) {
        const int row = m_wifiTable->rowCount();
        m_wifiTable->insertRow(row);

        auto* ssid_item = new QTableWidgetItem(net.ssid);
        if (net.isConnected) {
            QFont f = ssid_item->font();
            f.setBold(true);
            ssid_item->setFont(f);
            ssid_item->setText(QStringLiteral("%1 *").arg(net.ssid));
        }
        m_wifiTable->setItem(row, 0, ssid_item);

        m_wifiTable->setItem(row, KWifiColumnBssid, new QTableWidgetItem(net.bssid));

        auto* signal_item = new QTableWidgetItem(QStringLiteral("%1 dBm").arg(net.rssiDbm));
        if (net.rssiDbm >= kWifiStrongSignalDbm) {
            signal_item->setForeground(QColor(ui::kColorSuccess));
        } else if (net.rssiDbm >= kWifiUsableSignalDbm) {
            signal_item->setForeground(QColor(ui::kColorWarning));
        } else {
            signal_item->setForeground(QColor(ui::kColorError));
        }
        m_wifiTable->setItem(row, KWifiColumnSignal, signal_item);

        m_wifiTable->setItem(row,
                             KWifiColumnQuality,
                             new QTableWidgetItem(QStringLiteral("%1%").arg(net.signalQuality)));
        m_wifiTable->setItem(row,
                             KWifiColumnChannel,
                             new QTableWidgetItem(QString::number(net.channelNumber)));
        m_wifiTable->setItem(row, KWifiColumnBand, new QTableWidgetItem(net.band));
        m_wifiTable->setItem(row, KWifiColumnSecurity, new QTableWidgetItem(net.authentication));
        m_wifiTable->setItem(row, KWifiColumnVendor, new QTableWidgetItem(net.apVendor));
    }

    m_wifiTable->setSortingEnabled(true);
}

// -- Connections --

void NetworkDiagnosticPanel::onStartConnectionMonitor() {
    Q_ASSERT(m_connStartBtn);
    Q_ASSERT(m_connStopBtn);
    m_connStartBtn->setEnabled(false);
    m_connStopBtn->setEnabled(true);
    m_connEmptyState->setLoading(tr("Loading active connections..."));

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
    m_connEmptyState->clearLoading();
}

void NetworkDiagnosticPanel::onConnectionsUpdated(QVector<ConnectionInfo> connections) {
    Q_ASSERT(m_connTable);
    Q_ASSERT(m_connSummaryLabel);
    // Lift the loading overlay on the first monitor update so data is visible.
    m_connEmptyState->clearLoading();
    m_connTable->setSortingEnabled(false);
    m_connTable->setRowCount(static_cast<int>(connections.size()));

    int tcp_count = 0;
    int udp_count = 0;
    int established = 0;

    auto set_or_create = [this](int row, int col, const QString& text) {
        if (auto* existing = m_connTable->item(row, col)) {
            existing->setText(text);
        } else {
            m_connTable->setItem(row, col, new QTableWidgetItem(text));
        }
    };

    auto state_color = [](const QString& state) -> QColor {
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
        is_tcp ? ++tcp_count : ++udp_count;
        if (conn.state == QStringLiteral("ESTABLISHED")) {
            ++established;
        }

        set_or_create(i,
                      KConnectionColumnProtocol,
                      is_tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"));
        set_or_create(i, KConnectionColumnLocalAddress, conn.localAddress);
        set_or_create(i, KConnectionColumnLocalPort, QString::number(conn.localPort));
        set_or_create(i, KConnectionColumnRemoteAddress, conn.remoteAddress);
        set_or_create(i, KConnectionColumnRemotePort, QString::number(conn.remotePort));

        set_or_create(i, KConnectionColumnState, conn.state);
        m_connTable->item(i, KConnectionColumnState)->setForeground(state_color(conn.state));

        set_or_create(i, KConnectionColumnProcess, conn.processName);
    }

    m_connTable->setSortingEnabled(true);
    m_connSummaryLabel->setText(QStringLiteral("Total: %1 | TCP: %2 | UDP: %3 | Established: %4")
                                    .arg(connections.size())
                                    .arg(tcp_count)
                                    .arg(udp_count)
                                    .arg(established));
}

// -- Firewall --

void NetworkDiagnosticPanel::onAuditFirewall() {
    m_fwAuditBtn->setEnabled(false);
    m_fwRuleTable->setRowCount(0);
    m_fwConflictText->clear();
    m_fwGapText->clear();
    m_fwSummaryLabel->setText(tr("Auditing..."));
    m_fwRuleEmptyState->setLoading(tr("Running firewall audit..."));
    m_controller->auditFirewall();
}

void NetworkDiagnosticPanel::onFirewallAuditComplete(QVector<FirewallRule> rules,
                                                     QVector<FirewallConflict> conflicts,
                                                     QVector<FirewallGap> gaps) {
    m_cachedFwRules = rules;
    filterFirewallRules();
    m_fwAuditBtn->setEnabled(true);
    m_fwRuleEmptyState->clearLoading();

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
    if (dir_idx == kFirewallFilterOutboundIndex && direction != FirewallRule::Direction::Outbound) {
        return false;
    }
    return true;
}

bool matchesActionFilter(FirewallRule::Action action, int act_idx) {
    if (act_idx == 1 && action != FirewallRule::Action::Allow) {
        return false;
    }
    if (act_idx == kFirewallFilterBlockIndex && action != FirewallRule::Action::Block) {
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
    case FirewallRule::Protocol::Other:
        return QStringLiteral("Other");
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
    std::ranges::copy_if(m_cachedFwRules, std::back_inserter(filtered), matches);

    populateFirewallTable(filtered);
}

void NetworkDiagnosticPanel::populateFirewallTable(const QVector<FirewallRule>& filtered) {
    Q_ASSERT(m_fwRuleTable);

    m_fwRuleTable->setSortingEnabled(false);
    m_fwRuleTable->setRowCount(static_cast<int>(filtered.size()));

    for (int i = 0; i < filtered.size(); ++i) {
        const auto& r = filtered[i];
        auto* enabled_item = new QTableWidgetItem(r.enabled ? tr("Yes") : tr("No"));
        if (!r.enabled) {
            enabled_item->setForeground(QColor(ui::kColorTextMuted));
        }
        m_fwRuleTable->setItem(i, KFirewallColumnEnabled, enabled_item);
        m_fwRuleTable->setItem(i, KFirewallColumnName, new QTableWidgetItem(r.name));

        const auto dir_str = (r.direction == FirewallRule::Direction::Inbound) ? tr("Inbound")
                                                                               : tr("Outbound");
        m_fwRuleTable->setItem(i, KFirewallColumnDirection, new QTableWidgetItem(dir_str));

        auto* action_item = new QTableWidgetItem(
            (r.action == FirewallRule::Action::Allow) ? tr("Allow") : tr("Block"));
        action_item->setForeground((r.action == FirewallRule::Action::Allow)
                                       ? QColor(ui::kColorSuccess)
                                       : QColor(ui::kColorError));
        m_fwRuleTable->setItem(i, KFirewallColumnAction, action_item);

        m_fwRuleTable->setItem(i,
                               KFirewallColumnProtocol,
                               new QTableWidgetItem(protocolToString(r.protocol)));
        m_fwRuleTable->setItem(i, KFirewallColumnLocalPorts, new QTableWidgetItem(r.localPorts));
        m_fwRuleTable->setItem(i, KFirewallColumnRemotePorts, new QTableWidgetItem(r.remotePorts));
        m_fwRuleTable->setItem(i,
                               KFirewallColumnApplication,
                               new QTableWidgetItem(r.applicationPath));
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
    m_shareEmptyState->setLoading(tr("Enumerating network shares..."));
    m_controller->discoverShares(hostname);
}

void NetworkDiagnosticPanel::onSharesDiscovered(QVector<NetworkShareInfo> shares) {
    Q_ASSERT(m_shareDiscoverBtn);
    Q_ASSERT(m_shareTable);
    m_shareDiscoverBtn->setEnabled(true);
    m_shareEmptyState->clearLoading();

    m_shareTable->setRowCount(static_cast<int>(shares.size()));

    for (int i = 0; i < shares.size(); ++i) {
        const auto& s = shares[i];
        m_shareTable->setItem(i, KShareColumnName, new QTableWidgetItem(s.shareName));

        auto type_str = QStringLiteral("Disk");
        switch (s.type) {
        case NetworkShareInfo::ShareType::Printer:
            type_str = QStringLiteral("Printer");
            break;
        case NetworkShareInfo::ShareType::Device:
            type_str = QStringLiteral("Device");
            break;
        case NetworkShareInfo::ShareType::IPC:
            type_str = QStringLiteral("IPC");
            break;
        case NetworkShareInfo::ShareType::Special:
            type_str = QStringLiteral("Special");
            break;
        default:
            break;
        }
        m_shareTable->setItem(i, KShareColumnType, new QTableWidgetItem(type_str));

        auto* read_item = new QTableWidgetItem(s.canRead ? tr("Yes") : tr("No"));
        read_item->setForeground(s.canRead ? QColor(ui::kColorSuccess) : QColor(ui::kColorError));
        m_shareTable->setItem(i, KShareColumnRead, read_item);

        auto* write_item = new QTableWidgetItem(s.canWrite ? tr("Yes") : tr("No"));
        write_item->setForeground(s.canWrite ? QColor(ui::kColorSuccess) : QColor(ui::kColorError));
        m_shareTable->setItem(i, KShareColumnWrite, write_item);

        m_shareTable->setItem(i, KShareColumnRemark, new QTableWidgetItem(s.remark));
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
        Q_EMIT statusMessage(tr("Enter the target device IP address"), sak::kTimerStatusMessageMs);
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

void NetworkDiagnosticPanel::onLanTransferProgress(double current_mbps,
                                                   double elapsed_sec,
                                                   qint64 total_bytes) {
    m_lanResultLabel->setText(QStringLiteral("Running: %1 Mbps | %2 s | %3 MB transferred")
                                  .arg(current_mbps, 0, 'f', 1)
                                  .arg(elapsed_sec, 0, 'f', 0)
                                  .arg(static_cast<double>(total_bytes) / sak::kBytesPerMBf,
                                       0,
                                       'f',
                                       kDecimalPrecisionOne));
}

void NetworkDiagnosticPanel::onLanTransferComplete(LanTransferResult result) {
    Q_ASSERT(m_lanTestBtn);
    Q_ASSERT(m_lanResultLabel);
    m_lanTestBtn->setEnabled(true);
    m_lanResultLabel->setText(
        QStringLiteral("<b>%1 Complete</b><br>"
                       "Remote: %2<br>"
                       "Transferred: %3 MB in %4 s<br>"
                       "Average Speed: <b>%5 Mbps</b> (%6 MB/s)<br>"
                       "Peak Speed: %7 Mbps")
            .arg(result.isUpload ? tr("Upload") : tr("Download"))
            // remoteAddress is a peer-supplied string echoed into a rich-text label.
            .arg(result.remoteAddress.toHtmlEscaped())
            .arg(static_cast<double>(result.bytesTransferred) / sak::kBytesPerMBf,
                 0,
                 'f',
                 kDecimalPrecisionOne)
            .arg(result.durationSec, 0, 'f', 1)
            .arg(result.avgSpeedMbps, 0, 'f', 1)
            .arg(result.avgSpeedMbps / kBitsPerByteF, 0, 'f', kDecimalPrecisionOne)
            .arg(result.peakSpeedMbps, 0, 'f', 1));
}

// -- Controller State --

void NetworkDiagnosticPanel::onStateChanged(int new_state) {
    Q_UNUSED(new_state);
    // With concurrent operations, per-operation button management is handled
    // by operationFinished and the individual completion signal handlers.
    // This slot is kept for backward compatibility but does not re-enable
    // all buttons globally -- that would interfere with other running ops.
}

void NetworkDiagnosticPanel::resetDiagnosticButtons(int finished_state) {
    using S = NetworkDiagnosticController::State;
    const auto state = static_cast<S>(finished_state);

    // Also lift any loading overlay here: this is the shared finished sink reached on
    // success, error, and cancel, so the overlay can never be left stranded.
    switch (state) {
    case S::ScanningAdapters:
        m_refreshBtn->setEnabled(true);
        m_adapterEmptyState->clearLoading();
        break;
    case S::RunningPing:
        m_pingStartBtn->setEnabled(true);
        m_pingStopBtn->setEnabled(false);
        m_pingEmptyState->clearLoading();
        break;
    case S::RunningTraceroute:
        m_traceStartBtn->setEnabled(true);
        m_traceStopBtn->setEnabled(false);
        m_traceEmptyState->clearLoading();
        break;
    case S::RunningMtr:
        m_mtrStartBtn->setEnabled(true);
        m_mtrStopBtn->setEnabled(false);
        m_mtrEmptyState->clearLoading();
        break;
    case S::RunningDnsQuery:
        m_dnsQueryBtn->setEnabled(true);
        m_dnsReverseBtn->setEnabled(true);
        m_dnsCompareBtn->setEnabled(true);
        m_dnsFlushBtn->setEnabled(true);
        m_dnsEmptyState->clearLoading();
        break;
    case S::ScanningPorts:
        m_portStartBtn->setEnabled(true);
        m_portStopBtn->setEnabled(false);
        m_portEmptyState->clearLoading();
        break;
    default:
        break;
    }
}

void NetworkDiagnosticPanel::resetToolButtons(int finished_state) {
    using S = NetworkDiagnosticController::State;
    const auto state = static_cast<S>(finished_state);

    switch (state) {
    case S::RunningBandwidthTest:
        m_bwTestBtn->setEnabled(true);
        m_httpSpeedBtn->setEnabled(true);
        m_bwResultLabel->clear();
        m_httpSpeedLabel->clear();
        break;
    case S::ScanningWiFi:
        m_wifiScanBtn->setEnabled(true);
        m_wifiContBtn->setEnabled(true);
        m_wifiStopBtn->setEnabled(false);
        m_wifiEmptyState->clearLoading();
        break;
    case S::MonitoringConnections:
        m_connStartBtn->setEnabled(true);
        m_connStopBtn->setEnabled(false);
        m_connEmptyState->clearLoading();
        break;
    case S::AuditingFirewall:
        m_fwAuditBtn->setEnabled(true);
        m_fwRuleEmptyState->clearLoading();
        break;
    case S::BrowsingShares:
        m_shareDiscoverBtn->setEnabled(true);
        m_shareEmptyState->clearLoading();
        break;
    case S::RunningLanTransfer:
        m_lanTestBtn->setEnabled(true);
        break;
    default:
        break;
    }
}

void NetworkDiagnosticPanel::onOperationFinished(int finished_state) {
    resetDiagnosticButtons(finished_state);
    resetToolButtons(finished_state);
}

void NetworkDiagnosticPanel::onError(QString error) {
    Q_ASSERT(m_bwResultLabel);
    Q_ASSERT(m_httpSpeedLabel);
    Q_EMIT logOutput(QStringLiteral("[ERROR] %1").arg(error));
    Q_EMIT statusMessage(error, sak::kTimerStatusDefaultMs);

    // Errors from workers with missing completion signals will still
    // get cleaned up by QThread::finished -> removeOperation -> operationFinished.
    // No need to force-reset all buttons here.
}

// ===================================================================
// Quick Actions -- Reset Network
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
    // A network reset is destructive (Winsock/TCP-IP/firewall/adapter reset, dropped
    // leases) and requires a reboot, so confirm before running it -- it must not fire
    // from a single click with no prompt.
    auto confirm = sak::showQuestionLogged(
        this,
        tr("Reset Network"),
        tr("Reset the network stack now?\n\n"
           "This resets Winsock, TCP/IP, the Windows Firewall, and network adapters, and "
           "may drop your connection until you reboot."),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
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
            cells << ((item != nullptr) ? item->text() : QString());
        }
        text += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
    }
    QApplication::clipboard()->setText(text.trimmed());
    Q_EMIT statusMessage(tr("Copied %1 row(s) to clipboard").arg(rows.size()),
                         sak::kTimerBroadcastMs);
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
        headers << ((item != nullptr) ? item->text() : QString());
    }
    text += headers.join(QLatin1Char('\t')) + QLatin1Char('\n');

    // Data rows
    for (int row = 0; row < table->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < table->columnCount(); ++col) {
            auto* item = table->item(row, col);
            cells << ((item != nullptr) ? item->text() : QString());
        }
        text += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
    }
    QApplication::clipboard()->setText(text.trimmed());
    Q_EMIT statusMessage(tr("Copied %1 row(s) to clipboard").arg(table->rowCount()),
                         sak::kTimerBroadcastMs);
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
        Q_EMIT statusMessage(
            tr("Could not open '%1' for CSV export: %2").arg(path, file.errorString()),
            sak::kTimerStatusMessageMs);
        return;
    }

    QTextStream out(&file);
    // Header
    QStringList headers;
    for (int col = 0; col < table->columnCount(); ++col) {
        auto* item = table->horizontalHeaderItem(col);
        headers << QStringLiteral("\"%1\"").arg((item != nullptr) ? item->text() : QString());
    }
    out << headers.join(QLatin1Char(',')) << "\n";

    // Data
    for (int row = 0; row < table->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < table->columnCount(); ++col) {
            auto* item = table->item(row, col);
            QString cell_text = (item != nullptr) ? item->text() : QString();
            cell_text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            cells << QStringLiteral("\"%1\"").arg(cell_text);
        }
        out << cells.join(QLatin1Char(',')) << "\n";
    }

    Q_EMIT statusMessage(tr("Exported %1 rows to %2").arg(table->rowCount()).arg(path),
                         sak::kTimerStatusMessageMs);
    Q_EMIT logOutput(QStringLiteral("Exported results to %1").arg(path));
}

void NetworkDiagnosticPanel::copyTableCellValue(QTableWidget* table, int column) {
    Q_ASSERT(table);
    const int row = table->currentRow();
    if (row < 0) {
        return;
    }
    auto* item = table->item(row, column);
    if ((item != nullptr) && !item->text().isEmpty()) {
        QApplication::clipboard()->setText(item->text());
        Q_EMIT statusMessage(tr("Copied: %1").arg(item->text()), sak::kTimerBroadcastMs);
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
        menu.addAction(tr("Copy RTT"), this, [this]() {
            copyTableCellValue(m_pingTable, KPingColumnRtt);
        });
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
            copyTableCellValue(m_traceTable, KTraceColumnHostname);
        });
        menu.addSeparator();

        auto* ip_item = m_traceTable->item(m_traceTable->currentRow(), 1);
        if ((ip_item != nullptr) && !ip_item->text().isEmpty() &&
            ip_item->text() != QStringLiteral("*")) {
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
        menu.addAction(tr("Copy Loss %"), this, [this]() {
            copyTableCellValue(m_mtrTable, KMtrColumnLoss);
        });
        menu.addSeparator();

        auto* ip_item = m_mtrTable->item(m_mtrTable->currentRow(), 1);
        if ((ip_item != nullptr) && !ip_item->text().isEmpty() &&
            ip_item->text() != QStringLiteral("*")) {
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
        menu.addAction(tr("Copy Answers"), this, [this]() {
            copyTableCellValue(m_dnsTable, KDnsColumnAnswers);
        });
        menu.addAction(tr("Copy Server"), this, [this]() {
            copyTableCellValue(m_dnsTable, KDnsColumnServer);
        });
        menu.addSeparator();

        auto* answer_item = m_dnsTable->item(m_dnsTable->currentRow(), KDnsColumnAnswers);
        if ((answer_item != nullptr) && !answer_item->text().isEmpty()) {
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
        menu.addAction(tr("Copy Service"), this, [this]() {
            copyTableCellValue(m_portTable, KPortColumnService);
        });
        menu.addAction(tr("Copy Banner"), this, [this]() {
            copyTableCellValue(m_portTable, KPortColumnBanner);
        });
        menu.addSeparator();
        menu.addAction(tr("Copy Port:Service"), this, [this]() {
            const int row = m_portTable->currentRow();
            auto* port_item = m_portTable->item(row, 0);
            auto* svc_item = m_portTable->item(row, KPortColumnService);
            if (port_item) {
                QString text = port_item->text();
                if (svc_item && !svc_item->text().isEmpty()) {
                    text += QStringLiteral(" (%1)").arg(svc_item->text());
                }
                QApplication::clipboard()->setText(text);
                Q_EMIT statusMessage(tr("Copied: %1").arg(text), sak::kTimerBroadcastMs);
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
            const int row = m_wifiTable->currentRow();
            auto* sig = m_wifiTable->item(row, KWifiColumnSignal);
            auto* qual = m_wifiTable->item(row, KWifiColumnQuality);
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
            const int row = m_wifiTable->currentRow();
            auto* ch = m_wifiTable->item(row, KWifiColumnChannel);
            auto* band = m_wifiTable->item(row, KWifiColumnBand);
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
            copyTableCellValue(m_connTable, KConnectionColumnRemoteAddress);
        });
        menu.addAction(tr("Copy Remote Address:Port"), this, [this]() {
            const int row = m_connTable->currentRow();
            auto* addr = m_connTable->item(row, KConnectionColumnRemoteAddress);
            auto* port = m_connTable->item(row, KConnectionColumnRemotePort);
            if (addr) {
                QString text = addr->text();
                if (port && !port->text().isEmpty()) {
                    text += QLatin1Char(':') + port->text();
                }
                QApplication::clipboard()->setText(text);
                Q_EMIT statusMessage(tr("Copied: %1").arg(text), sak::kTimerBroadcastMs);
            }
        });
        menu.addAction(tr("Copy Process"), this, [this]() {
            copyTableCellValue(m_connTable, KConnectionColumnProcess);
        });
        menu.addSeparator();

        auto* remote_item = m_connTable->item(m_connTable->currentRow(),
                                              KConnectionColumnRemoteAddress);
        if ((remote_item != nullptr) && !remote_item->text().isEmpty() &&
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
                m_toolTabs->setCurrentIndex(kToolTabDnsIndex);
                onDnsReverseLookup();
            });
        }
    }

    addCommonTableActions(menu, m_connTable, QStringLiteral("connections_results.csv"));
    menu.exec(m_connTable->viewport()->mapToGlobal(pos));
}

void NetworkDiagnosticPanel::copyFirewallPorts() {
    const int row = m_fwRuleTable->currentRow();
    auto* local = m_fwRuleTable->item(row, KFirewallColumnLocalPorts);
    auto* remote = m_fwRuleTable->item(row, KFirewallColumnRemotePorts);
    QStringList parts;
    if ((local != nullptr) && !local->text().isEmpty()) {
        parts << QStringLiteral("Local: %1").arg(local->text());
    }
    if ((remote != nullptr) && !remote->text().isEmpty()) {
        parts << QStringLiteral("Remote: %1").arg(remote->text());
    }
    if (!parts.isEmpty()) {
        QApplication::clipboard()->setText(parts.join(QStringLiteral(", ")));
    }
}

void NetworkDiagnosticPanel::copyFirewallRuleDetails() {
    const int row = m_fwRuleTable->currentRow();
    QStringList details;
    for (int col = 0; col < m_fwRuleTable->columnCount(); ++col) {
        auto* header = m_fwRuleTable->horizontalHeaderItem(col);
        auto* item = m_fwRuleTable->item(row, col);
        if ((header != nullptr) && (item != nullptr)) {
            details << QStringLiteral("%1: %2").arg(header->text(), item->text());
        }
    }
    QApplication::clipboard()->setText(details.join(QLatin1Char('\n')));
    Q_EMIT statusMessage(tr("Copied full rule details"), sak::kTimerBroadcastMs);
}

void NetworkDiagnosticPanel::showFirewallContextMenu(const QPoint& pos) {
    if (m_fwRuleTable->rowCount() == 0) {
        return;
    }

    QMenu menu(this);

    if (m_fwRuleTable->currentRow() >= 0) {
        menu.addAction(tr("Copy Rule Name"), this, [this]() {
            copyTableCellValue(m_fwRuleTable, KFirewallColumnName);
        });
        menu.addAction(tr("Copy Application"), this, [this]() {
            copyTableCellValue(m_fwRuleTable, KFirewallColumnApplication);
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
            copyTableCellValue(m_shareTable, KShareColumnName);
        });

        auto* share_item = m_shareTable->item(m_shareTable->currentRow(), KShareColumnName);
        const QString host = m_shareHostname->text().trimmed();
        if ((share_item != nullptr) && !host.isEmpty()) {
            const QString unc_path = QStringLiteral("\\\\%1\\%2").arg(host, share_item->text());
            menu.addAction(tr("Copy UNC Path"), this, [this, unc_path]() {
                QApplication::clipboard()->setText(unc_path);
                Q_EMIT statusMessage(tr("Copied: %1").arg(unc_path), sak::kTimerBroadcastMs);
            });
            menu.addAction(tr("Open in Explorer"), this, [this, unc_path]() {
                if (!sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"), {unc_path})) {
                    Q_EMIT logOutput(tr("[ERROR] Could not launch Explorer"));
                }
            });
        }

        menu.addAction(tr("Copy Remark"), this, [this]() {
            copyTableCellValue(m_shareTable, KShareColumnRemark);
        });
    }

    addCommonTableActions(menu, m_shareTable, QStringLiteral("network_shares.csv"));
    menu.exec(m_shareTable->viewport()->mapToGlobal(pos));
}

}  // namespace sak
