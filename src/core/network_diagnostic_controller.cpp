// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_controller.cpp
/// @brief Orchestrates all network diagnostic workers and manages state

#include "sak/network_diagnostic_controller.h"

#include "sak/active_connections_monitor.h"
#include "sak/bandwidth_tester.h"
#include "sak/connectivity_tester.h"
#include "sak/dns_diagnostic_tool.h"
#include "sak/ethernet_config_manager.h"
#include "sak/firewall_rule_auditor.h"
#include "sak/network_adapter_inspector.h"
#include "sak/network_diagnostic_report_generator.h"
#include "sak/network_share_browser.h"
#include "sak/port_scanner.h"
#include "sak/wifi_analyzer.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace sak {

NetworkDiagnosticController::NetworkDiagnosticController(QObject* parent) : QObject(parent) {
    // Create all worker objects — they live in this thread initially
    // Long-running operations use runOnThread() to execute on a worker thread.
    m_adapterInspector = std::make_unique<NetworkAdapterInspector>();
    m_connectivityTester = std::make_unique<ConnectivityTester>();
    m_dnsTool = std::make_unique<DnsDiagnosticTool>();
    m_portScanner = std::make_unique<PortScanner>();
    m_bandwidthTester = std::make_unique<BandwidthTester>();
    m_wifiAnalyzer = std::make_unique<WiFiAnalyzer>();
    m_connectionMonitor = std::make_unique<ActiveConnectionsMonitor>();
    m_firewallAuditor = std::make_unique<FirewallRuleAuditor>();
    m_shareBrowser = std::make_unique<NetworkShareBrowser>();
    m_reportGenerator = std::make_unique<NetworkDiagnosticReportGenerator>();
    m_ethernetConfigManager = std::make_unique<EthernetConfigManager>(this);

    connectWorkerSignals();
}

NetworkDiagnosticController::~NetworkDiagnosticController() {
    cancel();
    cleanupThread();

    // Ensure workers with timers are stopped
    m_connectionMonitor->stopMonitoring();
    m_wifiAnalyzer->stopContinuousScan();
}

NetworkDiagnosticController::State NetworkDiagnosticController::currentState() const {
    return m_state;
}

void NetworkDiagnosticController::setState(State s) {
    if (m_state != s) {
        m_state = s;
        Q_EMIT stateChanged(static_cast<int>(s));
    }
}

void NetworkDiagnosticController::runOnThread(std::function<void()> work, State operationState) {
    Q_ASSERT(m_workerThread);
    if (m_state != State::Idle && m_state != State::MonitoringConnections) {
        Q_EMIT errorOccurred(QStringLiteral("Another operation is in progress"));
        return;
    }

    cleanupThread();
    setState(operationState);

    m_workerThread = new QThread(this);

    // Move the actual work to a lambda that runs on the new thread.
    // NOTE: No context object → DirectConnection → runs in emitting thread (worker).
    // Capture thread pointer locally to avoid racing with cleanupThread().
    QThread* workerThread = m_workerThread;
    QObject::connect(m_workerThread, &QThread::started, [workerThread, work]() {
        work();
        workerThread->quit();  // Exit event loop so thread finishes
    });

    // Auto-cleanup when thread finishes
    QObject::connect(m_workerThread, &QThread::finished, this, [this]() {
        if (m_state != State::MonitoringConnections) {
            setState(State::Idle);
        }
    });

    m_workerThread->start();
}

void NetworkDiagnosticController::cleanupThread() {
    Q_ASSERT(m_workerThread);
    if (m_workerThread) {
        if (m_workerThread->isRunning()) {
            m_workerThread->quit();
            if (!m_workerThread->wait(5000)) {
                m_workerThread->terminate();
                m_workerThread->wait(2000);
            }
        }
        m_workerThread->deleteLater();
        m_workerThread = nullptr;
    }
}

void NetworkDiagnosticController::connectWorkerSignals() {
    connectAdapterInspectorSignals();
    connectConnectivityTesterSignals();
    connectDnsToolSignals();
    connectPortScannerSignals();
    connectBandwidthTesterSignals();
    connectWifiAnalyzerSignals();
    connectConnectionMonitorSignals();
    connectFirewallAuditorSignals();
    connectShareBrowserSignals();
    connectReportGeneratorSignals();
    connectEthernetConfigManagerSignals();
}

void NetworkDiagnosticController::connectAdapterInspectorSignals() {
    Q_ASSERT(m_adapterInspector);
    connect(
        m_adapterInspector.get(),
        &NetworkAdapterInspector::scanComplete,
        this,
        [this](QVector<NetworkAdapterInfo> adapters) {
            m_cachedAdapters = adapters;
            Q_EMIT adaptersScanComplete(adapters);
            Q_EMIT logOutput(
                QStringLiteral("Adapter scan complete: %1 adapters found").arg(adapters.size()));
            setState(State::Idle);
        });
    connect(m_adapterInspector.get(),
            &NetworkAdapterInspector::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectConnectivityTesterSignals() {
    Q_ASSERT(m_connectivityTester);
    connect(m_connectivityTester.get(),
            &ConnectivityTester::pingReply,
            this,
            &NetworkDiagnosticController::pingReplyReceived);
    connect(m_connectivityTester.get(),
            &ConnectivityTester::pingComplete,
            this,
            [this](PingResult result) {
                m_cachedPing = result;
                Q_EMIT pingComplete(result);
                Q_EMIT logOutput(QStringLiteral("Ping complete: %1/%2 received, avg %3 ms")
                                     .arg(result.received)
                                     .arg(result.sent)
                                     .arg(result.avgRtt, 0, 'f', 1));
                setState(State::Idle);
            });

    connect(m_connectivityTester.get(),
            &ConnectivityTester::tracerouteHop,
            this,
            &NetworkDiagnosticController::tracerouteHopReceived);
    connect(m_connectivityTester.get(),
            &ConnectivityTester::tracerouteComplete,
            this,
            [this](TracerouteResult result) {
                m_cachedTraceroute = result;
                Q_EMIT tracerouteComplete(result);
                Q_EMIT logOutput(QStringLiteral("Traceroute to %1 complete: %2 hops")
                                     .arg(result.target)
                                     .arg(result.hops.size()));
                setState(State::Idle);
            });

    connect(m_connectivityTester.get(),
            &ConnectivityTester::mtrUpdate,
            this,
            &NetworkDiagnosticController::mtrUpdateReceived);
    connect(m_connectivityTester.get(),
            &ConnectivityTester::mtrComplete,
            this,
            [this](MtrResult result) {
                Q_EMIT mtrComplete(result);
                Q_EMIT logOutput(QStringLiteral("MTR to %1 complete: %2 cycles")
                                     .arg(result.target)
                                     .arg(result.totalCycles));
                setState(State::Idle);
            });

    connect(m_connectivityTester.get(),
            &ConnectivityTester::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectDnsToolSignals() {
    Q_ASSERT(m_dnsTool);
    Q_ASSERT(!m_cachedDns.isEmpty());
    connect(
        m_dnsTool.get(), &DnsDiagnosticTool::queryComplete, this, [this](DnsQueryResult result) {
            m_cachedDns.append(result);
            Q_EMIT dnsQueryComplete(result);
            Q_EMIT logOutput(QStringLiteral("DNS query for %1 (%2): %3 answers in %4 ms")
                                 .arg(result.queryName, result.recordType)
                                 .arg(result.answers.size())
                                 .arg(result.responseTimeMs, 0, 'f', 1));
            setState(State::Idle);
        });
    connect(m_dnsTool.get(),
            &DnsDiagnosticTool::comparisonComplete,
            this,
            [this](DnsServerComparison comparison) {
                Q_EMIT dnsComparisonComplete(comparison);
                Q_EMIT logOutput(
                    QStringLiteral("DNS comparison complete: %1 servers, "
                                   "agreement: %2")
                        .arg(comparison.results.size())
                        .arg(comparison.allAgree ? QStringLiteral("YES") : QStringLiteral("NO")));
                setState(State::Idle);
            });
    connect(m_dnsTool.get(),
            &DnsDiagnosticTool::dnsCacheResults,
            this,
            &NetworkDiagnosticController::dnsCacheResults);
    connect(m_dnsTool.get(),
            &DnsDiagnosticTool::dnsCacheFlushed,
            this,
            &NetworkDiagnosticController::dnsCacheFlushed);
    connect(m_dnsTool.get(),
            &DnsDiagnosticTool::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectPortScannerSignals() {
    Q_ASSERT(m_portScanner);
    connect(m_portScanner.get(),
            &PortScanner::portScanned,
            this,
            &NetworkDiagnosticController::portScannedResult);
    connect(m_portScanner.get(),
            &PortScanner::scanProgress,
            this,
            &NetworkDiagnosticController::portScanProgress);
    connect(m_portScanner.get(),
            &PortScanner::scanComplete,
            this,
            [this](QVector<PortScanResult> results) {
                m_cachedPortScan = results;
                Q_EMIT portScanComplete(results);
                int openCount = 0;
                for (const auto& r : results) {
                    if (r.state == PortScanResult::State::Open) {
                        ++openCount;
                    }
                }
                Q_EMIT logOutput(QStringLiteral("Port scan complete: %1 ports scanned, %2 open")
                                     .arg(results.size())
                                     .arg(openCount));
                setState(State::Idle);
            });
    connect(m_portScanner.get(),
            &PortScanner::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectBandwidthTesterSignals() {
    Q_ASSERT(m_bandwidthTester);
    connect(m_bandwidthTester.get(),
            &BandwidthTester::serverStarted,
            this,
            &NetworkDiagnosticController::iperfServerStarted);
    connect(m_bandwidthTester.get(),
            &BandwidthTester::serverStopped,
            this,
            &NetworkDiagnosticController::iperfServerStopped);
    connect(m_bandwidthTester.get(),
            &BandwidthTester::testProgress,
            this,
            &NetworkDiagnosticController::bandwidthProgress);
    connect(m_bandwidthTester.get(),
            &BandwidthTester::testComplete,
            this,
            [this](BandwidthTestResult result) {
                m_cachedBandwidth = result;
                Q_EMIT bandwidthComplete(result);
                Q_EMIT logOutput(QStringLiteral("Bandwidth test: DL %1 Mbps, UL %2 Mbps")
                                     .arg(result.downloadMbps, 0, 'f', 2)
                                     .arg(result.uploadMbps, 0, 'f', 2));
                setState(State::Idle);
            });
    connect(m_bandwidthTester.get(),
            &BandwidthTester::httpSpeedTestComplete,
            this,
            [this](double dl, double ul, double latency) {
                Q_EMIT httpSpeedComplete(dl, ul, latency);
                Q_EMIT logOutput(QStringLiteral("HTTP speed test: DL %1 Mbps, UL %2 Mbps, "
                                                "latency %3 ms")
                                     .arg(dl, 0, 'f', 2)
                                     .arg(ul, 0, 'f', 2)
                                     .arg(latency, 0, 'f', 1));
                setState(State::Idle);
            });
    connect(m_bandwidthTester.get(),
            &BandwidthTester::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectWifiAnalyzerSignals() {
    Q_ASSERT(m_wifiAnalyzer);
    connect(m_wifiAnalyzer.get(),
            &WiFiAnalyzer::scanComplete,
            this,
            [this](QVector<WiFiNetworkInfo> networks) {
                m_cachedWifi = networks;
                Q_EMIT wifiScanComplete(networks);

                auto channels = WiFiAnalyzer::calculateChannelUtilization(networks);
                Q_EMIT wifiChannelUtilization(channels);

                Q_EMIT logOutput(
                    QStringLiteral("WiFi scan complete: %1 networks found").arg(networks.size()));
                if (m_state == State::ScanningWiFi) {
                    setState(State::Idle);
                }
            });
    connect(m_wifiAnalyzer.get(),
            &WiFiAnalyzer::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectConnectionMonitorSignals() {
    connect(m_connectionMonitor.get(),
            &ActiveConnectionsMonitor::connectionsUpdated,
            this,
            [this](QVector<ConnectionInfo> connections) {
                m_cachedConnections = connections;
                Q_EMIT connectionsUpdated(connections);
            });
    connect(m_connectionMonitor.get(),
            &ActiveConnectionsMonitor::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectFirewallAuditorSignals() {
    Q_ASSERT(m_firewallAuditor);
    connect(m_firewallAuditor.get(),
            &FirewallRuleAuditor::auditComplete,
            this,
            [this](QVector<FirewallRule> rules,
                   QVector<FirewallConflict> conflicts,
                   QVector<FirewallGap> gaps) {
                m_cachedFirewallRules = rules;
                m_cachedFirewallConflicts = conflicts;
                m_cachedFirewallGaps = gaps;
                Q_EMIT firewallAuditComplete(rules, conflicts, gaps);
                Q_EMIT logOutput(QStringLiteral("Firewall audit: %1 rules, %2 conflicts, %3 gaps")
                                     .arg(rules.size())
                                     .arg(conflicts.size())
                                     .arg(gaps.size()));
                setState(State::Idle);
            });
    connect(m_firewallAuditor.get(),
            &FirewallRuleAuditor::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectShareBrowserSignals() {
    Q_ASSERT(m_shareBrowser);
    connect(m_shareBrowser.get(),
            &NetworkShareBrowser::discoveryComplete,
            this,
            [this](QVector<NetworkShareInfo> shares) {
                m_cachedShares = shares;
                Q_EMIT sharesDiscovered(shares);
                Q_EMIT logOutput(
                    QStringLiteral("Share discovery: %1 shares found").arg(shares.size()));
                setState(State::Idle);
            });
    connect(m_shareBrowser.get(),
            &NetworkShareBrowser::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectReportGeneratorSignals() {
    connect(m_reportGenerator.get(),
            &NetworkDiagnosticReportGenerator::reportGenerated,
            this,
            [this](QString path) {
                Q_EMIT reportGenerated(path);
                Q_EMIT logOutput(QStringLiteral("Report generated: %1").arg(path));
                setState(State::Idle);
            });
    connect(m_reportGenerator.get(),
            &NetworkDiagnosticReportGenerator::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

void NetworkDiagnosticController::connectEthernetConfigManagerSignals() {
    connect(m_ethernetConfigManager.get(),
            &EthernetConfigManager::logOutput,
            this,
            &NetworkDiagnosticController::logOutput);
    connect(m_ethernetConfigManager.get(),
            &EthernetConfigManager::errorOccurred,
            this,
            &NetworkDiagnosticController::errorOccurred);
}

// ═══════════════════════════════════════════════════════════════════
// Diagnostic Methods
// ═══════════════════════════════════════════════════════════════════

void NetworkDiagnosticController::scanAdapters() {
    Q_EMIT statusMessage(QStringLiteral("Scanning network adapters..."), 0);
    runOnThread([this]() { m_adapterInspector->scan(); }, State::ScanningAdapters);
}

void NetworkDiagnosticController::ping(
    const QString& target, int count, int intervalMs, int timeoutMs, int packetSize, int ttl) {
    if (target.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Ping target cannot be empty"));
        return;
    }

    Q_EMIT statusMessage(QStringLiteral("Pinging %1...").arg(target), 0);
    Q_EMIT logOutput(QStringLiteral("Starting ping to %1 (%2 packets, %3 ms timeout)")
                         .arg(target)
                         .arg(count)
                         .arg(timeoutMs));

    ConnectivityTester::PingConfig config;
    config.target = target;
    config.count = count;
    config.intervalMs = intervalMs;
    config.timeoutMs = timeoutMs;
    config.packetSizeBytes = packetSize;
    config.ttl = ttl;

    runOnThread([this, config]() { m_connectivityTester->ping(config); }, State::RunningPing);
}

void NetworkDiagnosticController::traceroute(
    const QString& target, int maxHops, int timeoutMs, int probesPerHop, bool resolveHostnames) {
    if (target.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Traceroute target cannot be empty"));
        return;
    }

    Q_EMIT statusMessage(QStringLiteral("Tracing route to %1...").arg(target), 0);
    Q_EMIT logOutput(
        QStringLiteral("Starting traceroute to %1 (max %2 hops)").arg(target).arg(maxHops));

    ConnectivityTester::TracerouteConfig config;
    config.target = target;
    config.maxHops = maxHops;
    config.timeoutMs = timeoutMs;
    config.probesPerHop = probesPerHop;
    config.resolveHostnames = resolveHostnames;

    runOnThread([this, config]() { m_connectivityTester->traceroute(config); },
                State::RunningTraceroute);
}

void NetworkDiagnosticController::mtr(
    const QString& target, int cycles, int intervalMs, int maxHops, int timeoutMs) {
    if (target.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("MTR target cannot be empty"));
        return;
    }

    Q_EMIT statusMessage(QStringLiteral("Running MTR to %1...").arg(target), 0);
    Q_EMIT logOutput(QStringLiteral("Starting MTR to %1 (%2 cycles)").arg(target).arg(cycles));

    ConnectivityTester::MtrConfig config;
    config.target = target;
    config.cycles = cycles;
    config.intervalMs = intervalMs;
    config.maxHops = maxHops;
    config.timeoutMs = timeoutMs;

    runOnThread([this, config]() { m_connectivityTester->mtr(config); }, State::RunningMtr);
}

void NetworkDiagnosticController::dnsQuery(const QString& hostname,
                                           const QString& recordType,
                                           const QString& dnsServer) {
    if (hostname.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("DNS hostname cannot be empty"));
        return;
    }

    Q_EMIT statusMessage(QStringLiteral("Querying DNS for %1...").arg(hostname), 0);

    runOnThread(
        [this, hostname, recordType, dnsServer]() {
            m_dnsTool->query(hostname, recordType, dnsServer);
        },
        State::RunningDnsQuery);
}

void NetworkDiagnosticController::dnsReverseLookup(const QString& ipAddress,
                                                   const QString& dnsServer) {
    if (ipAddress.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("IP address cannot be empty"));
        return;
    }

    Q_EMIT statusMessage(QStringLiteral("Reverse DNS lookup for %1...").arg(ipAddress), 0);

    runOnThread([this, ipAddress, dnsServer]() { m_dnsTool->reverseLookup(ipAddress, dnsServer); },
                State::RunningDnsQuery);
}

void NetworkDiagnosticController::dnsCompare(const QString& hostname,
                                             const QString& recordType,
                                             const QStringList& servers) {
    if (hostname.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("DNS hostname cannot be empty"));
        return;
    }

    Q_EMIT statusMessage(QStringLiteral("Comparing DNS servers for %1...").arg(hostname), 0);
    Q_EMIT logOutput(
        QStringLiteral("Comparing %1 DNS servers for %2").arg(servers.size()).arg(hostname));

    runOnThread(
        [this, hostname, recordType, servers]() {
            m_dnsTool->compareServers(hostname, recordType, servers);
        },
        State::RunningDnsQuery);
}

void NetworkDiagnosticController::dnsInspectCache() {
    Q_EMIT statusMessage(QStringLiteral("Inspecting DNS cache..."), 0);

    runOnThread([this]() { m_dnsTool->inspectDnsCache(); }, State::RunningDnsQuery);
}

void NetworkDiagnosticController::dnsFlushCache() {
    Q_EMIT statusMessage(QStringLiteral("Flushing DNS cache..."), 0);

    runOnThread([this]() { m_dnsTool->flushDnsCache(); }, State::RunningDnsQuery);
}

void NetworkDiagnosticController::scanPorts(const QString& target,
                                            const QVector<uint16_t>& ports,
                                            uint16_t rangeStart,
                                            uint16_t rangeEnd,
                                            int timeoutMs,
                                            int maxConcurrent,
                                            bool grabBanners) {
    if (target.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Port scan target cannot be empty"));
        return;
    }

    PortScanner::ScanConfig config;
    config.target = target;
    config.ports = ports;
    config.portRangeStart = rangeStart;
    config.portRangeEnd = rangeEnd;
    config.timeoutMs = timeoutMs;
    config.maxConcurrent = maxConcurrent;
    config.grabBanners = grabBanners;

    Q_EMIT statusMessage(QStringLiteral("Scanning ports on %1...").arg(target), 0);
    Q_EMIT logOutput(QStringLiteral("Starting port scan on %1").arg(target));

    runOnThread([this, config]() { m_portScanner->scan(config); }, State::ScanningPorts);
}

void NetworkDiagnosticController::startIperfServer(uint16_t port) {
    m_bandwidthTester->startIperfServer(port);
}

void NetworkDiagnosticController::stopIperfServer() {
    m_bandwidthTester->stopIperfServer();
}

bool NetworkDiagnosticController::isIperfServerRunning() const {
    return m_bandwidthTester->isServerRunning();
}

void NetworkDiagnosticController::runBandwidthTest(
    const QString& serverAddr, uint16_t port, int durationSec, int streams, bool bidir, bool udp) {
    if (serverAddr.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Bandwidth test server address cannot be empty"));
        return;
    }

    BandwidthTester::IperfConfig config;
    config.serverAddress = serverAddr;
    config.port = port;
    config.durationSec = durationSec;
    config.parallelStreams = streams;
    config.bidirectional = bidir;
    config.udpMode = udp;

    Q_EMIT statusMessage(QStringLiteral("Running bandwidth test against %1...").arg(serverAddr), 0);
    Q_EMIT logOutput(QStringLiteral("Starting iPerf3 test to %1:%2 (%3s, %4 streams)")
                         .arg(serverAddr)
                         .arg(port)
                         .arg(durationSec)
                         .arg(streams));

    runOnThread([this, config]() { m_bandwidthTester->runIperfTest(config); },
                State::RunningBandwidthTest);
}

void NetworkDiagnosticController::runHttpSpeedTest() {
    Q_EMIT statusMessage(QStringLiteral("Running HTTP speed test..."), 0);
    Q_EMIT logOutput(QStringLiteral("Starting HTTP-based speed test"));

    runOnThread([this]() { m_bandwidthTester->runHttpSpeedTest(); }, State::RunningBandwidthTest);
}

void NetworkDiagnosticController::scanWiFi() {
    Q_EMIT statusMessage(QStringLiteral("Scanning WiFi networks..."), 0);

    runOnThread([this]() { m_wifiAnalyzer->scan(); }, State::ScanningWiFi);
}

void NetworkDiagnosticController::startContinuousWiFiScan(int intervalMs) {
    Q_EMIT logOutput(
        QStringLiteral("Starting continuous WiFi scan (interval: %1 ms)").arg(intervalMs));
    m_wifiAnalyzer->startContinuousScan(intervalMs);
}

void NetworkDiagnosticController::stopContinuousWiFiScan() {
    m_wifiAnalyzer->stopContinuousScan();
    Q_EMIT logOutput(QStringLiteral("Continuous WiFi scan stopped"));
}

bool NetworkDiagnosticController::isWiFiAvailable() const {
    return m_wifiAnalyzer->isWiFiAvailable();
}

void NetworkDiagnosticController::startConnectionMonitor(
    int refreshMs, bool showTcp, bool showUdp, const QString& processFilter, uint16_t portFilter) {
    ActiveConnectionsMonitor::MonitorConfig config;
    config.refreshIntervalMs = refreshMs;
    config.showTcp = showTcp;
    config.showUdp = showUdp;
    config.filterProcessName = processFilter;
    config.filterPort = portFilter;

    Q_EMIT logOutput(QStringLiteral("Starting connection monitor (refresh: %1 ms)").arg(refreshMs));
    setState(State::MonitoringConnections);
    m_connectionMonitor->startMonitoring(config);
}

void NetworkDiagnosticController::stopConnectionMonitor() {
    m_connectionMonitor->stopMonitoring();
    Q_EMIT logOutput(QStringLiteral("Connection monitor stopped"));
    setState(State::Idle);
}

void NetworkDiagnosticController::auditFirewall() {
    Q_EMIT statusMessage(QStringLiteral("Auditing firewall rules..."), 0);
    Q_EMIT logOutput(QStringLiteral("Starting firewall audit"));

    runOnThread([this]() { m_firewallAuditor->fullAudit(); }, State::AuditingFirewall);
}

void NetworkDiagnosticController::discoverShares(const QString& hostname) {
    Q_ASSERT(!hostname.isEmpty());
    Q_ASSERT(m_shareBrowser);
    if (hostname.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Share discovery hostname cannot be empty"));
        return;
    }

    Q_EMIT statusMessage(QStringLiteral("Discovering shares on %1...").arg(hostname), 0);
    Q_EMIT logOutput(QStringLiteral("Discovering network shares on %1").arg(hostname));

    runOnThread([this, hostname]() { m_shareBrowser->discoverShares(hostname); },
                State::BrowsingShares);
}

// ── LAN File Transfer Speed Test ────────────────────────────────────────────

void NetworkDiagnosticController::startLanTransferServer(uint16_t port) {
    Q_ASSERT(m_lanTransferServer);
    if (m_lanTransferServerRunning.load(std::memory_order_acquire)) {
        Q_EMIT errorOccurred(QStringLiteral("LAN transfer server is already running"));
        return;
    }

    if (!m_lanTransferServer) {
        m_lanTransferServer = new QTcpServer(this);
    }

    if (!m_lanTransferServer->listen(QHostAddress::Any, port)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to start LAN transfer server "
                                            "on port %1: %2")
                                 .arg(port)
                                 .arg(m_lanTransferServer->errorString()));
        return;
    }

    m_lanTransferServerRunning.store(true, std::memory_order_release);
    Q_EMIT lanTransferServerStarted(port);
    Q_EMIT logOutput(QStringLiteral("LAN transfer server listening on port %1").arg(port));

    connect(m_lanTransferServer, &QTcpServer::newConnection, this, [this]() {
        auto* socket = m_lanTransferServer->nextPendingConnection();
        if (!socket) {
            return;
        }
        handleLanClientConnection(socket);
    });
}

void NetworkDiagnosticController::handleLanClientConnection(QTcpSocket* socket) {
    Q_ASSERT(socket);
    Q_EMIT logOutput(QStringLiteral("LAN transfer: peer connected from %1")
                         .arg(socket->peerAddress().toString()));

    auto* timer = new QElapsedTimer();
    timer->start();
    auto* totalReceived = new qint64(0);
    auto* peakMbps = new double(0.0);
    auto* lastReportTime = new qint64(0);
    auto* lastReportBytes = new qint64(0);
    auto* speedSamples = new QVector<double>();

    connect(socket,
            &QTcpSocket::readyRead,
            this,
            [this,
             socket,
             timer,
             totalReceived,
             peakMbps,
             lastReportTime,
             lastReportBytes,
             speedSamples]() {
                qint64 bytes = socket->bytesAvailable();
                QByteArray received_data = socket->read(bytes);
                *totalReceived += received_data.size();

                qint64 elapsed = timer->elapsed();
                constexpr qint64 kReportIntervalMs = 1000;
                if (elapsed - *lastReportTime >= kReportIntervalMs) {
                    qint64 deltaBytes = *totalReceived - *lastReportBytes;
                    double deltaSec = (elapsed - *lastReportTime) / 1000.0;
                    double currentMbps = deltaSec > 0 ? (deltaBytes * 8.0 / (deltaSec * 1e6)) : 0.0;
                    *peakMbps = std::max(*peakMbps, currentMbps);
                    speedSamples->append(currentMbps);
                    *lastReportTime = elapsed;
                    *lastReportBytes = *totalReceived;

                    Q_EMIT lanTransferProgress(currentMbps, elapsed / 1000.0, *totalReceived);
                }
            });

    connect(socket,
            &QTcpSocket::disconnected,
            this,
            [this,
             socket,
             timer,
             totalReceived,
             peakMbps,
             lastReportTime,
             lastReportBytes,
             speedSamples]() {
                handleLanClientDisconnected(socket,
                                            timer,
                                            totalReceived,
                                            peakMbps,
                                            lastReportTime,
                                            lastReportBytes,
                                            speedSamples);
            });
}

void NetworkDiagnosticController::handleLanClientDisconnected(QTcpSocket* socket,
                                                              QElapsedTimer* timer,
                                                              qint64* totalReceived,
                                                              double* peakMbps,
                                                              qint64* lastReportTime,
                                                              qint64* lastReportBytes,
                                                              QVector<double>* speedSamples) {
    Q_ASSERT(socket);
    Q_ASSERT(timer);

    double elapsed_sec = timer->elapsed() / 1000.0;

    LanTransferResult result;
    result.remoteAddress = socket->peerAddress().toString();
    result.port = socket->localPort();
    result.bytesTransferred = *totalReceived;
    result.durationSec = elapsed_sec;
    result.avgSpeedMbps = elapsed_sec > 0 ? (*totalReceived * 8.0 / (elapsed_sec * 1e6)) : 0.0;
    result.peakSpeedMbps = *peakMbps;
    result.isUpload = false;
    result.timestamp = QDateTime::currentDateTime();
    result.speedSamplesMbps = *speedSamples;

    Q_EMIT lanTransferComplete(result);
    Q_EMIT logOutput(QStringLiteral("LAN transfer receive complete: "
                                    "%1 MB in %2s (%3 Mbps avg)")
                         .arg(*totalReceived / (1024.0 * 1024.0), 0, 'f', 1)
                         .arg(elapsed_sec, 0, 'f', 1)
                         .arg(result.avgSpeedMbps, 0, 'f', 1));

    delete timer;
    delete totalReceived;
    delete peakMbps;
    delete lastReportTime;
    delete lastReportBytes;
    delete speedSamples;
    socket->deleteLater();
}

void NetworkDiagnosticController::stopLanTransferServer() {
    if (m_lanTransferServer && m_lanTransferServer->isListening()) {
        m_lanTransferServer->close();
    }
    m_lanTransferServerRunning.store(false, std::memory_order_release);
    Q_EMIT lanTransferServerStopped();
    Q_EMIT logOutput(QStringLiteral("LAN transfer server stopped"));
}

bool NetworkDiagnosticController::isLanTransferServerRunning() const {
    return m_lanTransferServerRunning.load(std::memory_order_acquire);
}

static QByteArray createPatternBuffer(int blockSizeKB) {
    const int blockSize = blockSizeKB * 1024;
    QByteArray buffer(blockSize, '\0');
    for (int idx = 0; idx < blockSize; ++idx) {
        buffer[idx] = static_cast<char>(idx & 0xFF);
    }
    return buffer;
}

static LanTransferResult buildLanTransferResult(const QString& addr,
                                                uint16_t port,
                                                qint64 totalSent,
                                                double elapsedSec,
                                                double peakMbps,
                                                const QVector<double>& samples) {
    LanTransferResult result;
    result.remoteAddress = addr;
    result.port = port;
    result.bytesTransferred = totalSent;
    result.durationSec = elapsedSec;
    result.avgSpeedMbps = elapsedSec > 0 ? (totalSent * 8.0 / (elapsedSec * 1e6)) : 0.0;
    result.peakSpeedMbps = peakMbps;
    result.isUpload = true;
    result.timestamp = QDateTime::currentDateTime();
    result.speedSamplesMbps = samples;
    return result;
}

void NetworkDiagnosticController::finalizeLanTransfer(QTcpSocket& socket,
                                                      const QString& targetAddr,
                                                      uint16_t port,
                                                      qint64 totalSent,
                                                      qint64 elapsedMs,
                                                      double peakMbps,
                                                      const QVector<double>& speedSamples) {
    Q_ASSERT(totalSent >= 0);
    Q_ASSERT(!targetAddr.isEmpty());

    socket.waitForBytesWritten(3000);
    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        socket.waitForDisconnected(3000);
    }

    double elapsed_sec = elapsedMs / 1000.0;
    auto result =
        buildLanTransferResult(targetAddr, port, totalSent, elapsed_sec, peakMbps, speedSamples);
    Q_EMIT lanTransferComplete(result);
    Q_EMIT logOutput(QStringLiteral("LAN transfer complete: "
                                    "%1 MB in %2s (%3 Mbps avg)")
                         .arg(totalSent / (1024.0 * 1024.0), 0, 'f', 1)
                         .arg(elapsed_sec, 0, 'f', 1)
                         .arg(result.avgSpeedMbps, 0, 'f', 1));
    Q_EMIT statusMessage(QStringLiteral("LAN transfer test complete"), 5000);
}

void NetworkDiagnosticController::runLanTransferTest(const QString& targetAddr,
                                                     uint16_t port,
                                                     int durationSec,
                                                     int blockSizeKB) {
    if (targetAddr.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Target address cannot be empty"));
        return;
    }
    if (durationSec < 1) {
        durationSec = 10;
    }
    if (blockSizeKB < 1) {
        blockSizeKB = 64;
    }

    Q_EMIT logOutput(QStringLiteral("Starting LAN transfer test to %1:%2 (%3s, %4 KB blocks)")
                         .arg(targetAddr)
                         .arg(port)
                         .arg(durationSec)
                         .arg(blockSizeKB));
    Q_EMIT statusMessage(QStringLiteral("Running LAN transfer speed test..."), 0);

    runOnThread(
        [this, targetAddr, port, durationSec, blockSizeKB]() {
            QTcpSocket socket;
            socket.connectToHost(targetAddr, port);

            if (!socket.waitForConnected(5000)) {
                Q_EMIT errorOccurred(QStringLiteral("Failed to connect to %1:%2 — %3. "
                                                    "Ensure the remote device is running "
                                                    "the LAN Transfer server.")
                                         .arg(targetAddr)
                                         .arg(port)
                                         .arg(socket.errorString()));
                return;
            }

            QByteArray sendBuffer = createPatternBuffer(blockSizeKB);
            const int blockSize = blockSizeKB * 1024;
            QElapsedTimer timer;
            timer.start();
            qint64 totalSent = 0;
            double peakMbps = 0.0;
            qint64 lastReportTime = 0;
            qint64 lastReportBytes = 0;
            QVector<double> speedSamples;
            const qint64 durationMs = static_cast<qint64>(durationSec) * 1000;

            while (timer.elapsed() < durationMs) {
                qint64 written = socket.write(sendBuffer);
                if (written < 0) {
                    Q_EMIT errorOccurred(
                        QStringLiteral("LAN transfer write error: %1").arg(socket.errorString()));
                    break;
                }
                totalSent += written;

                if ((totalSent % (blockSize * 16)) == 0) {
                    socket.waitForBytesWritten(100);
                }

                qint64 elapsed = timer.elapsed();
                if (elapsed - lastReportTime >= 1000) {
                    qint64 deltaBytes = totalSent - lastReportBytes;
                    double deltaSec = (elapsed - lastReportTime) / 1000.0;
                    double currentMbps = deltaSec > 0 ? (deltaBytes * 8.0 / (deltaSec * 1e6)) : 0.0;
                    peakMbps = std::max(peakMbps, currentMbps);
                    speedSamples.append(currentMbps);
                    lastReportTime = elapsed;
                    lastReportBytes = totalSent;
                    Q_EMIT lanTransferProgress(currentMbps, elapsed / 1000.0, totalSent);
                }
            }

            finalizeLanTransfer(
                socket, targetAddr, port, totalSent, timer.elapsed(), peakMbps, speedSamples);
        },
        State::Idle);
}

void NetworkDiagnosticController::generateReport(const QString& outputPath,
                                                 const QString& format,
                                                 const QString& technician,
                                                 const QString& ticket,
                                                 const QString& notes) {
    if (m_state != State::Idle && m_state != State::MonitoringConnections) {
        Q_EMIT errorOccurred(QStringLiteral("Another operation is in progress"));
        return;
    }

    // Populate report generator with cached data
    m_reportGenerator->setTechnicianName(technician);
    m_reportGenerator->setTicketNumber(ticket);
    m_reportGenerator->setNotes(notes);

    // Include all sections that have data
    QSet<NetworkDiagnosticReportGenerator::Section> sections;
    if (!m_cachedAdapters.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::AdapterConfig);
        m_reportGenerator->setAdapterData(m_cachedAdapters);
    }
    if (m_cachedPing.sent > 0) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::PingResults);
        m_reportGenerator->setPingData(m_cachedPing);
    }
    if (!m_cachedTraceroute.hops.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::TracerouteResults);
        m_reportGenerator->setTracerouteData(m_cachedTraceroute);
    }
    if (!m_cachedDns.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::DnsResults);
        m_reportGenerator->setDnsData(m_cachedDns);
    }
    if (!m_cachedPortScan.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::PortScanResults);
        m_reportGenerator->setPortScanData(m_cachedPortScan);
    }
    if (m_cachedBandwidth.downloadMbps > 0 || m_cachedBandwidth.uploadMbps > 0) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::BandwidthResults);
        m_reportGenerator->setBandwidthData(m_cachedBandwidth);
    }
    if (!m_cachedWifi.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::WiFiAnalysis);
        m_reportGenerator->setWiFiData(m_cachedWifi);
    }
    if (!m_cachedFirewallRules.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::FirewallAudit);
        m_reportGenerator->setFirewallData(m_cachedFirewallRules,
                                           m_cachedFirewallConflicts,
                                           m_cachedFirewallGaps);
    }
    if (!m_cachedConnections.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::ActiveConnections);
        m_reportGenerator->setConnectionData(m_cachedConnections);
    }
    if (!m_cachedShares.isEmpty()) {
        sections.insert(NetworkDiagnosticReportGenerator::Section::NetworkShares);
        m_reportGenerator->setShareData(m_cachedShares);
    }

    m_reportGenerator->setIncludedSections(sections);

    Q_EMIT statusMessage(QStringLiteral("Generating report..."), 0);

    // Use runOnThread to avoid blocking the main thread with file I/O
    const bool isJson = (format.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0);
    runOnThread(
        [this, outputPath, isJson]() {
            if (isJson) {
                m_reportGenerator->generateJson(outputPath);
            } else {
                m_reportGenerator->generateHtml(outputPath);
            }
        },
        State::GeneratingReport);
}

// ── Ethernet Config Backup/Restore ──────────────────────────────────────────

void NetworkDiagnosticController::backupEthernetSettings(const QString& adapterName,
                                                         const QString& filePath) {
    if (adapterName.isEmpty() || filePath.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Adapter name and file path are required for backup"));
        return;
    }

    Q_EMIT logOutput(QStringLiteral("Backing up settings for: %1").arg(adapterName));

    auto snapshot = m_ethernetConfigManager->captureSettings(adapterName);
    if (!snapshot.isValid()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to capture settings for adapter: %1").arg(adapterName));
        return;
    }

    if (m_ethernetConfigManager->saveToFile(snapshot, filePath)) {
        Q_EMIT ethernetBackupComplete(filePath);
        Q_EMIT statusMessage(QStringLiteral("Ethernet settings backed up to: %1").arg(filePath),
                             5000);
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to save backup to: %1").arg(filePath));
    }
}

void NetworkDiagnosticController::restoreEthernetSettings(const QString& filePath,
                                                          const QString& targetAdapter) {
    if (filePath.isEmpty() || targetAdapter.isEmpty()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Backup file and target adapter are required for restore"));
        return;
    }

    Q_EMIT logOutput(
        QStringLiteral("Restoring settings from: %1 to adapter: %2").arg(filePath, targetAdapter));

    auto snapshot = m_ethernetConfigManager->loadFromFile(filePath);
    if (!snapshot.isValid()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load backup from: %1").arg(filePath));
        return;
    }

    bool success = m_ethernetConfigManager->restoreSettings(snapshot, targetAdapter);
    Q_EMIT ethernetRestoreComplete(success);

    if (success) {
        Q_EMIT statusMessage(QStringLiteral("Ethernet settings restored successfully"), 5000);
    } else {
        Q_EMIT statusMessage(
            QStringLiteral("Some settings failed to restore — check log for details"), 5000);
    }
}

QStringList NetworkDiagnosticController::listEthernetAdapters() const {
    return m_ethernetConfigManager->listEthernetAdapters();
}

void NetworkDiagnosticController::cancel() {
    Q_ASSERT(m_connectivityTester);
    Q_ASSERT(m_dnsTool);
    m_connectivityTester->cancel();
    m_dnsTool->cancel();
    m_portScanner->cancel();
    m_bandwidthTester->cancel();
    m_firewallAuditor->cancel();
    m_shareBrowser->cancel();
    m_wifiAnalyzer->stopContinuousScan();
    m_connectionMonitor->stopMonitoring();
    if (m_lanTransferServerRunning.load(std::memory_order_acquire)) {
        stopLanTransferServer();
    }

    // If no worker thread is running (continuous monitors, etc.),
    // transition state to Idle directly since QThread::finished won't fire.
    if (!m_workerThread || !m_workerThread->isRunning()) {
        setState(State::Idle);
    }

    Q_EMIT logOutput(QStringLiteral("Operation cancelled"));
    Q_EMIT statusMessage(QStringLiteral("Cancelled"), 3000);
}

}  // namespace sak
