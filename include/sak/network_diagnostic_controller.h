// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_controller.h
/// @brief Orchestrates all network diagnostic workers and manages state

#pragma once

#include "sak/network_diagnostic_report_generator.h"
#include "sak/network_diagnostic_types.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>

class QHostAddress;
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

    /// @brief True only when no blocking operation is active (the passive connection
    ///        monitor is permitted to overlap). Deterministic -- does not consult the
    ///        order-dependent m_state.
    [[nodiscard]] bool canGenerateReport() const;

    // -- Adapter Inspection --
    void scanAdapters();

    // -- Connectivity --
    struct PingParams {
        QString target;
        int count{0};
        int interval_ms{0};
        int timeout_ms{0};
        int packet_size{0};
        int ttl{0};
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
        uint16_t range_start{0};
        uint16_t range_end{0};
        int timeout_ms{0};
        int max_concurrent{0};
        bool grab_banners{false};
    };
    void scanPorts(const PortScanParams& params);

    // -- Bandwidth --
    struct BandwidthTestParams {
        QString server_addr;
        uint16_t port{0};
        int duration_sec{0};
        int streams{0};
        bool bidirectional{false};
        bool udp{false};
    };
    /// @brief Start the iPerf3 server listening only on @p bind_address (see
    ///        BandwidthTester::startIperfServer).
    void startIperfServer(uint16_t port, const QHostAddress& bind_address);
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

    /// @brief Start a TCP server to receive data from a peer for speed measurement, listening
    ///        only on @p bind_address: one of this PC's adapter addresses, or loopback. A
    ///        wildcard (any address) is refused, so the server is never open on every network
    ///        the PC is on.
    void startLanTransferServer(uint16_t port, const QHostAddress& bind_address);

    /// @brief An address a server can be told to listen on, as the panel offers it.
    struct ServerListenChoice {
        QString label;              ///< "Ethernet - 10.0.0.5"
        QString address;            ///< "10.0.0.5"
        bool default_route{false};  ///< the adapter has an IPv4 gateway: the default route
    };

    /// @brief The IPv4 addresses of the connected adapters in @p adapters, in order, one choice
    ///        each: what the iPerf3 and LAN transfer servers may listen on. Pure; testable.
    [[nodiscard]] static QVector<ServerListenChoice> serverListenChoices(
        const QVector<NetworkAdapterInfo>& adapters);

    /// @brief Which of @p choices a server's address list starts on: @p remembered (the address
    ///        last used) when it is still offered, else the first adapter holding the default
    ///        route, else the first choice. -1 when there are none. Pure; testable.
    [[nodiscard]] static int preferredListenChoice(const QVector<ServerListenChoice>& choices,
                                                   const QString& remembered);

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
    /// @param adapterType The adapter's type as the adapter list names it, kept in the backup so
    ///        a restore can start on an adapter of the same kind.
    void backupEthernetSettings(const QString& adapterName,
                                const QString& filePath,
                                const QString& adapterType = {});

    /// @brief The adapter name and type a backup file was made from; both empty when the file
    ///        cannot be read (the reason goes to errorOccurred).
    [[nodiscard]] QPair<QString, QString> ethernetBackupIdentity(const QString& filePath);

    /// @brief Restore Ethernet adapter settings from a JSON backup file
    void restoreEthernetSettings(const QString& filePath, const QString& targetAdapter);

    /// @brief List available Ethernet adapter names
    [[nodiscard]] QStringList listEthernetAdapters() const;

    /// @brief What the Log says about a finished iPerf3 test. Pure.
    [[nodiscard]] static QString iperfLogLine(const BandwidthTestResult& result);
    /// @brief What the Log says about a finished HTTP speed test. Pure.
    [[nodiscard]] static QString httpSpeedLogLine(double downloadMbps,
                                                  double uploadMbps,
                                                  double latencyMs);

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

    /// @brief Report sections whose diagnostic was attempted this session (mapped from
    ///        m_attemptedOps). Subtracting the with-data sections yields the attempted-but-
    ///        empty set the report renders as an explicit "not run" marker.
    [[nodiscard]] QSet<NetworkDiagnosticReportGenerator::Section> attemptedReportSections() const;

    /// @brief Keep a finished HTTP speed test's numbers for the report.
    void cacheHttpSpeedResult(double downloadMbps, double uploadMbps, double latencyMs);

    State m_state = State::Idle;  ///< Legacy: first active op or Idle
    QSet<State> m_activeOps;      ///< Currently running operations
    QSet<State> m_attemptedOps;   ///< Every op attempted this session (for not-run report state)

    std::unique_ptr<NetworkAdapterInspector> m_adapterInspector;
    std::unique_ptr<ConnectivityTester> m_connectivityTester;
    std::unique_ptr<DnsDiagnosticTool> m_dnsTool;
    std::unique_ptr<PortScanner> m_portScanner;
    std::unique_ptr<BandwidthTester> m_bandwidthTester;
    std::unique_ptr<WiFiAnalyzer> m_wifiAnalyzer;
    int m_continuousWiFiIntervalMs{0};
    bool m_continuousWiFiRequested{false};  ///< cleared by stop; read when the first scan lands
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
    /// Runs @p work on a fresh worker thread; @p afterFinished (optional) runs on this thread once
    /// the worker has finished and the operation has been removed.
    void runOnThread(std::function<void()> work,
                     State operationState,
                     std::function<void()> afterFinished = {});
    /// Starts the periodic WiFi refresh after the continuous scan's first (worker) scan.
    void beginContinuousWiFiRefresh();
    void cleanupThread(State op);
    void cleanupAllThreads();
    void connectWorkerSignals();

    /// @brief Drop the cached result for @p op so a failed re-run cannot leave stale
    ///        prior-run data eligible for the next report.
    void clearCacheFor(State op);

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
        uint16_t port{};
        qint64 total_sent{};
        qint64 elapsed_ms{};
        double peak_mbps{};
        QVector<double> speed_samples;
    };

    /// @brief Finalize LAN transfer and emit results
    void finalizeLanTransfer(const LanTransferData& data);

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
