// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_controller.h
/// @brief Orchestrates all network diagnostic workers and manages state

#pragma once

#include "sak/network_diagnostic_report_generator.h"
#include "sak/network_diagnostic_types.h"

#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>

class QTcpServer;
class QTcpSocket;

namespace sak {

class NetworkAdapterInspector;
class ConnectivityTester;
class DnsDiagnosticTool;
class PortScanner;
class BandwidthTester;
class WiFiAnalyzer;
class ActiveConnectionsMonitor;
class FirewallRuleAuditor;
class NetworkShareBrowser;
class EthernetConfigManager;

/// @brief Orchestrates all network diagnostic components
///
/// State machine manages which diagnostic tool is currently active.
/// Creates worker threads for long-running operations and forwards
/// all results to the panel via signals.
class NetworkDiagnosticController : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        ScanningAdapters,
        RunningPing,
        RunningTraceroute,
        RunningMtr,
        RunningDnsQuery,
        ScanningPorts,
        RunningBandwidthTest,
        ScanningWiFi,
        MonitoringConnections,
        AuditingFirewall,
        BrowsingShares,
        RunningLanTransfer,
        GeneratingReport
    };

    explicit NetworkDiagnosticController(QObject* parent = nullptr);
    ~NetworkDiagnosticController() override;

    NetworkDiagnosticController(const NetworkDiagnosticController&) = delete;
    NetworkDiagnosticController& operator=(const NetworkDiagnosticController&) = delete;
    NetworkDiagnosticController(NetworkDiagnosticController&&) = delete;
    NetworkDiagnosticController& operator=(NetworkDiagnosticController&&) = delete;

    /// @brief Get current controller state (first active, or Idle)
    [[nodiscard]] State currentState() const;

    /// @brief Check if a specific operation is currently active
    [[nodiscard]] bool isOperationActive(State op) const;

    /// @brief Check if any operation is running
    [[nodiscard]] bool hasActiveOperations() const;

    // -- Adapter Inspection --
    void scanAdapters();

    // -- Connectivity --
    struct PingParams {
        QString target;
        int count;
        int interval_ms;
        int timeout_ms;
        int packet_size;
        int ttl;
    };
    void ping(const PingParams& params);
    void traceroute(
        const QString& target, int maxHops, int timeoutMs, int probesPerHop, bool resolveHostnames);
    void mtr(const QString& target, int cycles, int intervalMs, int maxHops, int timeoutMs);

    // -- DNS --
    void dnsQuery(const QString& hostname, const QString& recordType, const QString& dnsServer);
    void dnsReverseLookup(const QString& ipAddress, const QString& dnsServer);
    void dnsCompare(const QString& hostname, const QString& recordType, const QStringList& servers);
    void dnsInspectCache();
    void dnsFlushCache();

    // -- Port Scanning --
    struct PortScanParams {
        QString target;
        QVector<uint16_t> ports;
        uint16_t range_start;
        uint16_t range_end;
        int timeout_ms;
        int max_concurrent;
        bool grab_banners;
    };
    void scanPorts(const PortScanParams& params);

    // -- Bandwidth --
    struct BandwidthTestParams {
        QString server_addr;
        uint16_t port;
        int duration_sec;
        int streams;
        bool bidirectional;
        bool udp;
    };
    void startIperfServer(uint16_t port);
    void stopIperfServer();
    [[nodiscard]] bool isIperfServerRunning() const;
    void runBandwidthTest(const BandwidthTestParams& params);
    void runHttpSpeedTest();

    // -- WiFi --
    void scanWiFi();
    void startContinuousWiFiScan(int intervalMs);
    void stopContinuousWiFiScan();
    [[nodiscard]] bool isWiFiAvailable() const;

    // -- Connections --
    void startConnectionMonitor(int refreshMs,
                                bool showTcp,
                                bool showUdp,
                                const QString& processFilter,
                                uint16_t portFilter);
    void stopConnectionMonitor();

    // -- Firewall --
    void auditFirewall();

    // -- Shares --
    void discoverShares(const QString& hostname);

    // -- LAN File Transfer Speed Test --

    /// @brief Start a TCP server to receive data from a peer for speed measurement
    void startLanTransferServer(uint16_t port);

    /// @brief Stop the LAN transfer server
    void stopLanTransferServer();

    /// @brief Check if the LAN transfer server is running
    [[nodiscard]] bool isLanTransferServerRunning() const;

    /// @brief Run a LAN transfer speed test as a client sending data to a server
    void runLanTransferTest(const QString& targetAddr,
                            uint16_t port,
                            int durationSec,
                            int blockSizeKB);

    // -- Ethernet Config --

    /// @brief Backup the current Ethernet adapter settings to a JSON file
    void backupEthernetSettings(const QString& adapterName, const QString& filePath);

    /// @brief Restore Ethernet adapter settings from a JSON backup file
    void restoreEthernetSettings(const QString& filePath, const QString& targetAdapter);

    /// @brief List available Ethernet adapter names
    [[nodiscard]] QStringList listEthernetAdapters() const;

    // -- Report --
    void generateReport(const QString& outputPath,
                        const QString& format,
                        const QString& technician,
                        const QString& ticket,
                        const QString& notes);

    // -- Cancel --
    void cancel();

Q_SIGNALS:
    void stateChanged(int newState);
    void operationStarted(int operationState);
    void operationFinished(int operationState);
    void statusMessage(QString message, int timeout);
    void progressUpdated(int percent, QString status);

    // Component result signals
    void adaptersScanComplete(QVector<sak::NetworkAdapterInfo> adapters);
    void pingReplyReceived(sak::PingReply reply);
    void pingComplete(sak::PingResult result);
    void tracerouteHopReceived(sak::TracerouteHop hop);
    void tracerouteComplete(sak::TracerouteResult result);
    void mtrUpdateReceived(QVector<sak::MtrHopStats> hops, int cycle);
    void mtrComplete(sak::MtrResult result);
    void dnsQueryComplete(sak::DnsQueryResult result);
    void dnsComparisonComplete(sak::DnsServerComparison comparison);
    void dnsCacheResults(QVector<QPair<QString, QString>> entries);
    void dnsCacheFlushed();
    void portScannedResult(sak::PortScanResult result);
    void portScanProgress(int scanned, int total);
    void portScanComplete(QVector<sak::PortScanResult> results);
    void bandwidthProgress(double currentMbps, double elapsedSec, double totalSec);
    void bandwidthComplete(sak::BandwidthTestResult result);
    void iperfServerStarted(uint16_t port);
    void iperfServerStopped();
    void httpSpeedComplete(double downloadMbps, double uploadMbps, double latencyMs);
    void wifiScanComplete(QVector<sak::WiFiNetworkInfo> networks);
    void wifiChannelUtilization(QVector<sak::WiFiChannelUtilization> channels);
    void connectionsUpdated(QVector<sak::ConnectionInfo> connections);
    void firewallAuditComplete(QVector<sak::FirewallRule> rules,
                               QVector<sak::FirewallConflict> conflicts,
                               QVector<sak::FirewallGap> gaps);
    void sharesDiscovered(QVector<sak::NetworkShareInfo> shares);
    void lanTransferServerStarted(uint16_t port);
    void lanTransferServerStopped();
    void lanTransferProgress(double currentMbps, double elapsedSec, qint64 bytesTransferred);
    void lanTransferComplete(sak::LanTransferResult result);
    void ethernetBackupComplete(QString filePath);
    void ethernetRestoreComplete(bool success);
    void reportGenerated(QString path);
    void errorOccurred(QString error);
    void logOutput(QString message);

private:
    QSet<NetworkDiagnosticReportGenerator::Section> populateReportSections();
    void populateBasicReportSections(QSet<NetworkDiagnosticReportGenerator::Section>& sections);
    void populateAdvancedReportSections(QSet<NetworkDiagnosticReportGenerator::Section>& sections);

    State m_state = State::Idle;  ///< Legacy: first active op or Idle
    QSet<State> m_activeOps;      ///< Currently running operations

    std::unique_ptr<NetworkAdapterInspector> m_adapterInspector;
    std::unique_ptr<ConnectivityTester> m_connectivityTester;
    std::unique_ptr<DnsDiagnosticTool> m_dnsTool;
    std::unique_ptr<PortScanner> m_portScanner;
    std::unique_ptr<BandwidthTester> m_bandwidthTester;
    std::unique_ptr<WiFiAnalyzer> m_wifiAnalyzer;
    std::unique_ptr<ActiveConnectionsMonitor> m_connectionMonitor;
    std::unique_ptr<FirewallRuleAuditor> m_firewallAuditor;
    std::unique_ptr<NetworkShareBrowser> m_shareBrowser;
    std::unique_ptr<NetworkDiagnosticReportGenerator> m_reportGenerator;
    std::unique_ptr<EthernetConfigManager> m_ethernetConfigManager;

    QHash<State, QThread*> m_workerThreads;  ///< Per-operation threads

    // LAN transfer server state
    QTcpServer* m_lanTransferServer = nullptr;
    std::atomic<bool> m_lanTransferServerRunning{false};

    // Cached results for report generation
    QVector<NetworkAdapterInfo> m_cachedAdapters;
    PingResult m_cachedPing;
    TracerouteResult m_cachedTraceroute;
    QVector<DnsQueryResult> m_cachedDns;
    QVector<PortScanResult> m_cachedPortScan;
    BandwidthTestResult m_cachedBandwidth;
    QVector<WiFiNetworkInfo> m_cachedWifi;
    QVector<FirewallRule> m_cachedFirewallRules;
    QVector<FirewallConflict> m_cachedFirewallConflicts;
    QVector<FirewallGap> m_cachedFirewallGaps;
    QVector<ConnectionInfo> m_cachedConnections;
    QVector<NetworkShareInfo> m_cachedShares;

    void setState(State s);
    void addOperation(State op);
    void removeOperation(State op);
    bool isWorkerGroupBusy(State op) const;
    void runOnThread(std::function<void()> work, State operationState);
    void cleanupThread(State op);
    void cleanupAllThreads();
    void connectWorkerSignals();

    /// @brief Handle an incoming LAN transfer client connection
    void handleLanClientConnection(QTcpSocket* socket);

    /// Context for tracking a LAN client receive session
    struct LanClientContext {
        QElapsedTimer timer;
        qint64 total_received = 0;
        double peak_mbps = 0.0;
        qint64 last_report_time = 0;
        qint64 last_report_bytes = 0;
        QVector<double> speed_samples;
    };

    /// @brief Handle LAN transfer client disconnect and emit results
    void handleLanClientDisconnected(QTcpSocket* socket, LanClientContext* ctx);

    /// Transfer result data for finalization
    struct LanTransferData {
        QString target_addr;
        uint16_t port;
        qint64 total_sent;
        qint64 elapsed_ms;
        double peak_mbps;
        QVector<double> speed_samples;
    };

    /// @brief Finalize LAN transfer: disconnect socket, emit results
    void finalizeLanTransfer(QTcpSocket& socket, const LanTransferData& data);

    void connectAdapterInspectorSignals();
    void connectConnectivityTesterSignals();
    void connectDnsToolSignals();
    void connectPortScannerSignals();
    void connectBandwidthTesterSignals();
    void connectWifiAnalyzerSignals();
    void connectConnectionMonitorSignals();
    void connectFirewallAuditorSignals();
    void connectShareBrowserSignals();
    void connectReportGeneratorSignals();
    void connectEthernetConfigManagerSignals();
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::NetworkDiagnosticController>,
              "NetworkDiagnosticController must not be copyable.");
