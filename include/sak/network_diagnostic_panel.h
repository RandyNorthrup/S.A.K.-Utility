// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_panel.h
/// @brief Main UI panel for Network Diagnostics & Troubleshooting

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QWidget>

#include <memory>
#include <type_traits>

class QVBoxLayout;
class QGroupBox;
class QCheckBox;

namespace sak {

class NetworkDiagnosticController;
class LogToggleSwitch;

/// @brief Network Diagnostics & Troubleshooting Panel
///
/// Sub-tabbed interface with adapter inspector (always visible),
/// diagnostic tool tabs (Ping, Traceroute, MTR, DNS, Port Scan,
/// Bandwidth, WiFi, Connections, Firewall, Shares), and report
/// generation section.
class NetworkDiagnosticPanel : public QWidget {
    Q_OBJECT

public:
    explicit NetworkDiagnosticPanel(QWidget* parent = nullptr);
    ~NetworkDiagnosticPanel() override;

    NetworkDiagnosticPanel(const NetworkDiagnosticPanel&) = delete;
    NetworkDiagnosticPanel& operator=(const NetworkDiagnosticPanel&) = delete;
    NetworkDiagnosticPanel(NetworkDiagnosticPanel&&) = delete;
    NetworkDiagnosticPanel& operator=(NetworkDiagnosticPanel&&) = delete;

    /// @brief Access the log toggle switch for MainWindow connection
    [[nodiscard]] LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Adapter
    void onRefreshAdapters();
    void onAdaptersScanComplete(QVector<sak::NetworkAdapterInfo> adapters);
    void onAdapterSelectionChanged();
    void onCopyAdapterConfig();
    void onBackupEthernetSettings();
    void onRestoreEthernetSettings();

    // Ping
    void onStartPing();
    void onStopPing();
    void onPingReply(sak::PingReply reply);
    void onPingComplete(sak::PingResult result);

    // Traceroute
    void onStartTraceroute();
    void onStopTraceroute();
    void onTracerouteHop(sak::TracerouteHop hop);
    void onTracerouteComplete(sak::TracerouteResult result);

    // MTR
    void onStartMtr();
    void onStopMtr();
    void onMtrUpdate(QVector<sak::MtrHopStats> hops, int cycle);
    void onMtrComplete(sak::MtrResult result);

    // DNS
    void onDnsQuery();
    void onDnsReverseLookup();
    void onDnsCompare();
    void onDnsFlushCache();
    void onDnsQueryComplete(sak::DnsQueryResult result);
    void onDnsComparisonComplete(sak::DnsServerComparison comparison);

    // Port Scanner
    void onPortPresetChanged(int index);
    void onStartPortScan();
    void onStopPortScan();
    void onPortScanned(sak::PortScanResult result);
    void onPortScanComplete(QVector<sak::PortScanResult> results);
    void onPortScanProgress(int scanned, int total);

    // Bandwidth
    void onStartBandwidthTest();
    void onStartIperfServer();
    void onStopIperfServer();
    void onRunHttpSpeedTest();
    void onBandwidthComplete(sak::BandwidthTestResult result);
    void onHttpSpeedComplete(double down, double up, double latency);

    // WiFi
    void onScanWiFi();
    void onStartContinuousWiFi();
    void onStopContinuousWiFi();
    void onWiFiScanComplete(QVector<sak::WiFiNetworkInfo> networks);

    // Connections
    void onStartConnectionMonitor();
    void onStopConnectionMonitor();
    void onConnectionsUpdated(QVector<sak::ConnectionInfo> connections);

    // Firewall
    void onAuditFirewall();
    void onFirewallAuditComplete(QVector<sak::FirewallRule> rules,
                                 QVector<sak::FirewallConflict> conflicts,
                                 QVector<sak::FirewallGap> gaps);

    // Shares
    void onDiscoverShares();
    void onSharesDiscovered(QVector<sak::NetworkShareInfo> shares);

    // LAN Transfer
    void onStartLanTransferServer();
    void onStopLanTransferServer();
    void onRunLanTransferTest();
    void onLanTransferProgress(double currentMbps, double elapsedSec, qint64 totalBytes);
    void onLanTransferComplete(sak::LanTransferResult result);

    // Controller
    void onStateChanged(int newState);
    void onError(QString error);

private:
    // -- UI Setup --
    void setupUi();
    void setupKeyboardShortcuts();
    QWidget* createAdapterSection();
    void setupAdapterToolbar(QGroupBox* group, QVBoxLayout* layout);
    void setupAdapterTable(QGroupBox* group, QVBoxLayout* layout);
    void setupAdapterDetailLabel(QGroupBox* group, QVBoxLayout* layout);
    QWidget* createPingTab();
    void setupPingConfig(QWidget* widget, QVBoxLayout* layout);
    void setupPingControls(QWidget* widget, QVBoxLayout* layout);
    void setupPingResults(QWidget* widget, QVBoxLayout* layout);
    QWidget* createTracerouteTab();
    void setupTracerouteConfig(QWidget* widget, QVBoxLayout* layout);
    void setupTracerouteControls(QWidget* widget, QVBoxLayout* layout);
    void setupTracerouteResults(QWidget* widget, QVBoxLayout* layout);
    QWidget* createMtrTab();
    void setupMtrConfig(QWidget* widget, QVBoxLayout* layout);
    void setupMtrControls(QWidget* widget, QVBoxLayout* layout);
    void setupMtrResults(QWidget* widget, QVBoxLayout* layout);
    QWidget* createDnsTab();
    void setupDnsConfig(QWidget* widget, QVBoxLayout* layout);
    void setupDnsControls(QWidget* widget, QVBoxLayout* layout);
    void setupDnsResults(QWidget* widget, QVBoxLayout* layout);
    QWidget* createPortScanTab();
    void setupPortScanConfig(QWidget* widget, QVBoxLayout* layout);
    void setupPortScanControls(QWidget* widget, QVBoxLayout* layout);
    void setupPortScanResults(QWidget* widget, QVBoxLayout* layout);
    QWidget* createBandwidthTab();
    void setupBandwidthIperfConfig(QWidget* widget, QVBoxLayout* iperfLayout);
    void setupBandwidthIperfControls(QWidget* widget, QVBoxLayout* iperfLayout);
    void setupBandwidthIperfResults(QWidget* widget, QVBoxLayout* iperfLayout);
    void setupBandwidthHttpSection(QWidget* widget, QVBoxLayout* layout);
    QWidget* createWiFiTab();
    QWidget* createConnectionsTab();
    void setupConnectionsConfig(QWidget* widget, QVBoxLayout* layout);
    void setupConnectionsControls(QWidget* widget, QVBoxLayout* layout);
    void setupConnectionsTable(QWidget* widget, QVBoxLayout* layout);
    QWidget* createFirewallTab();
    void setupFirewallToolbar(QWidget* widget, QVBoxLayout* layout);
    void setupFirewallRuleTable(QWidget* widget, QVBoxLayout* layout);
    void setupFirewallAnalysis(QWidget* widget, QVBoxLayout* layout);
    QWidget* createSharesTab();
    QWidget* createLanTransferTab();
    QGroupBox* createLanServerGroup(QWidget* parent);
    QGroupBox* createLanClientGroup(QWidget* parent);
    void connectSignals();
    void connectUiSignals();
    void connectControllerCoreSignals();
    void connectControllerAdapterPingTraceMtrSignals();
    void connectControllerDnsPortSignals();
    void connectControllerBandwidthSignals();
    void connectControllerWifiConnectionsFirewallSharesSignals();
    void connectReportAndEthernetSignals();
    void connectFirewallFilterSignals();

    // -- Adapter detail formatting --
    [[nodiscard]] QString formatAdapterIdentity(const sak::NetworkAdapterInfo& adapter) const;
    [[nodiscard]] QString formatAdapterAddressing(const sak::NetworkAdapterInfo& adapter) const;
    [[nodiscard]] QString formatAdapterGatewayDns(const sak::NetworkAdapterInfo& adapter) const;
    [[nodiscard]] QString formatAdapterStatus(const sak::NetworkAdapterInfo& adapter) const;

    // -- Controller --
    std::unique_ptr<NetworkDiagnosticController> m_controller;
    LogToggleSwitch* m_logToggle = nullptr;  ///< Owned by layout hierarchy

    // All widget pointers below are owned by the Qt parent/layout hierarchy.
    // They are destroyed automatically when this panel is destroyed.

    // -- Adapter UI --
    QTableWidget* m_adapterTable = nullptr;
    QLabel* m_detailIdentity = nullptr;    ///< Name / Description / MAC
    QLabel* m_detailAddressing = nullptr;  ///< IPv4 / IPv6
    QLabel* m_detailGatewayDns = nullptr;  ///< Gateways / DNS
    QLabel* m_detailStatus = nullptr;      ///< DHCP / Speed / Status
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_copyConfigBtn = nullptr;
    QPushButton* m_backupEthernetBtn = nullptr;
    QPushButton* m_restoreEthernetBtn = nullptr;

    // -- Tool Tabs --
    QTabWidget* m_toolTabs = nullptr;

    // -- Ping UI --
    QLineEdit* m_pingTarget = nullptr;
    QSpinBox* m_pingCount = nullptr;
    QSpinBox* m_pingTimeout = nullptr;
    QSpinBox* m_pingInterval = nullptr;
    QSpinBox* m_pingPacketSize = nullptr;
    QTableWidget* m_pingTable = nullptr;
    QLabel* m_pingStatsLabel = nullptr;
    QPushButton* m_pingStartBtn = nullptr;
    QPushButton* m_pingStopBtn = nullptr;

    // -- Traceroute UI --
    QLineEdit* m_traceTarget = nullptr;
    QSpinBox* m_traceMaxHops = nullptr;
    QTableWidget* m_traceTable = nullptr;
    QLabel* m_traceStatusLabel = nullptr;
    QPushButton* m_traceStartBtn = nullptr;
    QPushButton* m_traceStopBtn = nullptr;

    // -- MTR UI --
    QLineEdit* m_mtrTarget = nullptr;
    QSpinBox* m_mtrCycles = nullptr;
    QTableWidget* m_mtrTable = nullptr;
    QLabel* m_mtrStatusLabel = nullptr;
    QPushButton* m_mtrStartBtn = nullptr;
    QPushButton* m_mtrStopBtn = nullptr;

    // -- DNS UI --
    QLineEdit* m_dnsHostname = nullptr;
    QComboBox* m_dnsRecordType = nullptr;
    QComboBox* m_dnsServer = nullptr;
    QTableWidget* m_dnsTable = nullptr;
    QLabel* m_dnsStatusLabel = nullptr;
    QPushButton* m_dnsQueryBtn = nullptr;    ///< Owned by layout hierarchy
    QPushButton* m_dnsReverseBtn = nullptr;  ///< Owned by layout hierarchy
    QPushButton* m_dnsCompareBtn = nullptr;  ///< Owned by layout hierarchy
    QPushButton* m_dnsFlushBtn = nullptr;    ///< Owned by layout hierarchy

    // -- Port Scanner UI --
    QLineEdit* m_portTarget = nullptr;
    QComboBox* m_portPreset = nullptr;
    QLineEdit* m_portCustomRange = nullptr;
    QSpinBox* m_portTimeout = nullptr;
    QSpinBox* m_portConcurrent = nullptr;
    QCheckBox* m_portBannerGrab = nullptr;
    QTableWidget* m_portTable = nullptr;
    QProgressBar* m_portProgress = nullptr;
    QLabel* m_portSummaryLabel = nullptr;
    QPushButton* m_portStartBtn = nullptr;
    QPushButton* m_portStopBtn = nullptr;

    // -- Bandwidth UI --
    QLineEdit* m_bwServerAddr = nullptr;
    QSpinBox* m_bwPort = nullptr;
    QSpinBox* m_bwDuration = nullptr;
    QSpinBox* m_bwStreams = nullptr;
    QCheckBox* m_bwBidirectional = nullptr;
    QLabel* m_bwResultLabel = nullptr;
    QPushButton* m_bwTestBtn = nullptr;
    QPushButton* m_bwServerStartBtn = nullptr;
    QPushButton* m_bwServerStopBtn = nullptr;
    QLabel* m_bwServerStatus = nullptr;
    QLabel* m_httpSpeedLabel = nullptr;
    QPushButton* m_httpSpeedBtn = nullptr;  ///< Owned by layout hierarchy

    // -- WiFi UI --
    QTableWidget* m_wifiTable = nullptr;
    QLabel* m_wifiChannelLabel = nullptr;
    QPushButton* m_wifiScanBtn = nullptr;
    QPushButton* m_wifiContBtn = nullptr;
    QPushButton* m_wifiStopBtn = nullptr;

    // -- Connection UI --
    QTableWidget* m_connTable = nullptr;
    QCheckBox* m_connShowTcp = nullptr;
    QCheckBox* m_connShowUdp = nullptr;
    QLineEdit* m_connProcessFilter = nullptr;
    QSpinBox* m_connRefreshRate = nullptr;
    QLabel* m_connSummaryLabel = nullptr;
    QPushButton* m_connStartBtn = nullptr;
    QPushButton* m_connStopBtn = nullptr;

    // -- Firewall UI --
    QTableWidget* m_fwRuleTable = nullptr;
    QLineEdit* m_fwSearchBox = nullptr;
    QComboBox* m_fwDirFilter = nullptr;
    QComboBox* m_fwActionFilter = nullptr;
    QLabel* m_fwSummaryLabel = nullptr;
    QPushButton* m_fwAuditBtn = nullptr;  ///< Owned by layout hierarchy
    QTextEdit* m_fwConflictText = nullptr;
    QTextEdit* m_fwGapText = nullptr;
    QVector<FirewallRule> m_cachedFwRules;  ///< Cached for local filtering

    void filterFirewallRules();
    void populateFirewallTable(const QVector<FirewallRule>& rules);

    // -- Shares UI --
    QLineEdit* m_shareHostname = nullptr;
    QTableWidget* m_shareTable = nullptr;
    QPushButton* m_shareDiscoverBtn = nullptr;  ///< Owned by layout hierarchy

    // -- LAN Transfer UI --
    QLineEdit* m_lanTarget = nullptr;
    QSpinBox* m_lanPort = nullptr;
    QSpinBox* m_lanDuration = nullptr;
    QSpinBox* m_lanBlockSize = nullptr;
    QLabel* m_lanResultLabel = nullptr;
    QPushButton* m_lanTestBtn = nullptr;
    QPushButton* m_lanServerStartBtn = nullptr;
    QPushButton* m_lanServerStopBtn = nullptr;
    QLabel* m_lanServerStatus = nullptr;

    // Cached data for UI
    QVector<NetworkAdapterInfo> m_adapters;
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::NetworkDiagnosticPanel>,
              "NetworkDiagnosticPanel must not be copyable.");
