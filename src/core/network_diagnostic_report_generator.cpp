// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_report_generator.cpp
/// @brief HTML/JSON diagnostic report generation

#include "sak/network_diagnostic_report_generator.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace sak {

namespace {
constexpr auto kReportTitle = "S.A.K. Utility — Network Diagnostic Report";
// CSS color tokens for report
constexpr auto kColorSuccess = "#16a34a";
constexpr auto kColorWarning = "#d97706";
constexpr auto kColorError = "#dc2626";
constexpr auto kColorInfo = "#2563eb";
}  // namespace

NetworkDiagnosticReportGenerator::NetworkDiagnosticReportGenerator(QObject* parent)
    : QObject(parent) {}

void NetworkDiagnosticReportGenerator::setTechnicianName(const QString& name) {
    m_technicianName = name;
}

void NetworkDiagnosticReportGenerator::setTicketNumber(const QString& ticket) {
    m_ticketNumber = ticket;
}

void NetworkDiagnosticReportGenerator::setNotes(const QString& notes) {
    m_notes = notes;
}

void NetworkDiagnosticReportGenerator::setIncludedSections(const QSet<Section>& sections) {
    m_sections = sections;
}

void NetworkDiagnosticReportGenerator::setAdapterData(const QVector<NetworkAdapterInfo>& adapters) {
    m_adapters = adapters;
}

void NetworkDiagnosticReportGenerator::setPingData(const PingResult& result) {
    m_pingResult = result;
}

void NetworkDiagnosticReportGenerator::setTracerouteData(const TracerouteResult& result) {
    m_tracerouteResult = result;
}

void NetworkDiagnosticReportGenerator::setDnsData(const QVector<DnsQueryResult>& results) {
    m_dnsResults = results;
}

void NetworkDiagnosticReportGenerator::setPortScanData(const QVector<PortScanResult>& results) {
    m_portScanResults = results;
}

void NetworkDiagnosticReportGenerator::setBandwidthData(const BandwidthTestResult& result) {
    m_bandwidthResult = result;
}

void NetworkDiagnosticReportGenerator::setWiFiData(const QVector<WiFiNetworkInfo>& networks) {
    m_wifiNetworks = networks;
}
void NetworkDiagnosticReportGenerator::setFirewallData(const QVector<FirewallRule>& rules,
                                                       const QVector<FirewallConflict>& conflicts,
                                                       const QVector<FirewallGap>& gaps) {
    m_firewallRules = rules;
    m_firewallConflicts = conflicts;
    m_firewallGaps = gaps;
}

void NetworkDiagnosticReportGenerator::setConnectionData(
    const QVector<ConnectionInfo>& connections) {
    m_connections = connections;
}

void NetworkDiagnosticReportGenerator::setShareData(const QVector<NetworkShareInfo>& shares) {
    m_shares = shares;
}

void NetworkDiagnosticReportGenerator::generateHtml(const QString& outputPath) {
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot write to %1").arg(outputPath));
        return;
    }

    QTextStream out(&file);
    out << toHtml();
    file.close();

    Q_EMIT reportGenerated(outputPath);
}

void NetworkDiagnosticReportGenerator::generateJson(const QString& outputPath) {
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot write to %1").arg(outputPath));
        return;
    }

    const QByteArray data = toJson().toUtf8();
    if (file.write(data) != data.size()) {
        Q_EMIT errorOccurred(QStringLiteral("Incomplete write to %1").arg(outputPath));
        return;
    }
    file.close();

    Q_EMIT reportGenerated(outputPath);
}

QString NetworkDiagnosticReportGenerator::toHtml() const {
    Q_ASSERT(!m_sections.empty());
    Q_ASSERT(!m_sections.isEmpty());
    QString html;
    html += buildHtmlHeader();

    appendHtmlSections(html);

    html += buildHtmlFooter();
    return html;
}

QString NetworkDiagnosticReportGenerator::buildHtmlHeader() const {
    Q_ASSERT(!m_technicianName.isEmpty());
    Q_ASSERT(!m_ticketNumber.isEmpty());
    QString html;
    html += QStringLiteral(
                "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
                "<meta charset=\"UTF-8\">\n"
                "<title>%1</title>\n"
                "<style>\n"
                "body { font-family: 'Segoe UI', Tahoma, sans-serif; margin: 40px; "
                "color: #334155; background: #f8fafc; }\n"
                "h1 { color: #0f172a; border-bottom: 3px solid #3b82f6; padding-bottom: 8px; }\n"
                "h2 { color: #1e293b; border-bottom: 1px solid #e2e8f0; padding-bottom: 4px; "
                "margin-top: 30px; }\n"
                "table { border-collapse: collapse; width: 100%%; margin: 10px 0; }\n"
                "th { background: #f1f5f9; padding: 8px 12px; text-align: left; "
                "border: 1px solid #cbd5e1; font-weight: 600; }\n"
                "td { padding: 6px 12px; border: 1px solid #e2e8f0; }\n"
                "tr:nth-child(even) { background: #f8fafc; }\n"
                ".success { color: %2; font-weight: 600; }\n"
                ".warning { color: %3; font-weight: 600; }\n"
                ".error { color: %4; font-weight: 600; }\n"
                ".info { color: %5; }\n"
                ".meta { color: #64748b; font-size: 0.9em; }\n"
                ".stat-box { background: #e0f2fe; padding: 12px; border-radius: 8px; "
                "margin: 8px 0; display: inline-block; }\n"
                "</style>\n</head>\n<body>\n"
                "<h1>%1</h1>\n")
                .arg(QLatin1String(kReportTitle),
                     QLatin1String(kColorSuccess),
                     QLatin1String(kColorWarning),
                     QLatin1String(kColorError),
                     QLatin1String(kColorInfo));

    // Meta information
    html += QStringLiteral("<div class=\"meta\">\n");
    html += QStringLiteral("<p><b>Generated:</b> %1</p>\n")
                .arg(QDateTime::currentDateTime().toString(Qt::ISODate));

    if (!m_technicianName.isEmpty()) {
        html +=
            QStringLiteral("<p><b>Technician:</b> %1</p>\n").arg(m_technicianName.toHtmlEscaped());
    }
    if (!m_ticketNumber.isEmpty()) {
        html += QStringLiteral("<p><b>Ticket:</b> %1</p>\n").arg(m_ticketNumber.toHtmlEscaped());
    }
    if (!m_notes.isEmpty()) {
        html += QStringLiteral("<p><b>Notes:</b> %1</p>\n").arg(m_notes.toHtmlEscaped());
    }
    html += QStringLiteral("</div>\n");

    return html;
}

QString NetworkDiagnosticReportGenerator::buildAdapterSection() const {
    QString html;
    html += QStringLiteral("<h2>Network Adapters</h2>\n");
    html += QStringLiteral(
        "<table>\n<tr><th>Name</th><th>Type</th><th>Status</th>"
        "<th>IP Address</th><th>MAC</th><th>Speed</th></tr>\n");

    for (const auto& a : m_adapters) {
        const auto status = a.isConnected
                                ? QStringLiteral("<span class=\"success\">Connected</span>")
                                : QStringLiteral("<span class=\"error\">Disconnected</span>");
        const auto ip = a.ipv4Addresses.isEmpty() ? QStringLiteral("—") : a.ipv4Addresses.first();
        const auto speed = a.linkSpeedBps > 0
                               ? QStringLiteral("%1 Mbps").arg(a.linkSpeedBps / 1'000'000)
                               : QStringLiteral("—");

        html += QStringLiteral(
                    "<tr><td>%1</td><td>%2</td><td>%3</td>"
                    "<td>%4</td><td>%5</td><td>%6</td></tr>\n")
                    .arg(a.name.toHtmlEscaped(), a.adapterType, status, ip, a.macAddress, speed);
    }
    html += QStringLiteral("</table>\n");
    return html;
}

QString NetworkDiagnosticReportGenerator::buildPingSection() const {
    QString html;
    html += QStringLiteral("<h2>Ping Results — %1</h2>\n").arg(m_pingResult.target.toHtmlEscaped());

    const auto lossClass = m_pingResult.lossPercent > 5.0
                               ? QStringLiteral("error")
                               : (m_pingResult.lossPercent > 0.0 ? QStringLiteral("warning")
                                                                 : QStringLiteral("success"));

    html += QStringLiteral(
                "<div class=\"stat-box\">"
                "Sent: %1 | Received: %2 | Lost: %3 "
                "(<span class=\"%4\">%5%%</span>)<br>"
                "Min: %6 ms | Max: %7 ms | Avg: %8 ms | Jitter: %9 ms"
                "</div>\n")
                .arg(m_pingResult.sent)
                .arg(m_pingResult.received)
                .arg(m_pingResult.lost)
                .arg(lossClass)
                .arg(m_pingResult.lossPercent, 0, 'f', 1)
                .arg(m_pingResult.minRtt, 0, 'f', 1)
                .arg(m_pingResult.maxRtt, 0, 'f', 1)
                .arg(m_pingResult.avgRtt, 0, 'f', 1)
                .arg(m_pingResult.jitter, 0, 'f', 2);

    return html;
}

QString NetworkDiagnosticReportGenerator::buildTracerouteSection() const {
    QString html;
    html +=
        QStringLiteral("<h2>Traceroute — %1</h2>\n").arg(m_tracerouteResult.target.toHtmlEscaped());

    html += QStringLiteral(
        "<table>\n<tr><th>Hop</th><th>IP</th><th>Hostname</th>"
        "<th>RTT 1</th><th>RTT 2</th><th>RTT 3</th><th>Avg</th></tr>\n");

    for (const auto& hop : m_tracerouteResult.hops) {
        if (hop.timedOut) {
            html += QStringLiteral(
                        "<tr><td>%1</td><td colspan=\"6\" class=\"warning\">"
                        "* * * Request timed out</td></tr>\n")
                        .arg(hop.hopNumber);
        } else {
            const QString host = hop.hostname.isEmpty() ? QStringLiteral("—")
                                                        : hop.hostname.toHtmlEscaped();
            html += QStringLiteral(
                        "<tr><td>%1</td><td>%2</td><td>%3</td>"
                        "<td>%4 ms</td><td>%5 ms</td><td>%6 ms</td>"
                        "<td>%7 ms</td></tr>\n")
                        .arg(hop.hopNumber)
                        .arg(hop.ipAddress.toHtmlEscaped())
                        .arg(host)
                        .arg(hop.rtt1Ms, 0, 'f', 1)
                        .arg(hop.rtt2Ms, 0, 'f', 1)
                        .arg(hop.rtt3Ms, 0, 'f', 1)
                        .arg(hop.avgRttMs, 0, 'f', 1);
        }
    }
    html += QStringLiteral("</table>\n");
    return html;
}

QString NetworkDiagnosticReportGenerator::buildDnsSection() const {
    QString html;
    html += QStringLiteral("<h2>DNS Query Results</h2>\n");

    for (const auto& r : m_dnsResults) {
        const auto statusClass = r.success ? QStringLiteral("success") : QStringLiteral("error");
        const auto statusText = r.success ? QStringLiteral("OK") : QStringLiteral("FAILED");

        html += QStringLiteral("<h3>%1 (%2) via %3 — <span class=\"%4\">%5</span> (%6 ms)</h3>\n")
                    .arg(r.queryName.toHtmlEscaped(),
                         r.recordType,
                         r.dnsServer.toHtmlEscaped(),
                         statusClass,
                         statusText)
                    .arg(r.responseTimeMs, 0, 'f', 1);

        if (!r.answers.isEmpty()) {
            html += QStringLiteral("<ul>\n");
            for (const auto& answer : r.answers) {
                html += QStringLiteral("<li>%1</li>\n").arg(answer.toHtmlEscaped());
            }
            html += QStringLiteral("</ul>\n");
        }
    }
    return html;
}

QString NetworkDiagnosticReportGenerator::buildPortScanSection() const {
    QString html;
    html += QStringLiteral("<h2>Port Scan Results</h2>\n");

    int openCount = 0;
    int closedCount = 0;
    int filteredCount = 0;
    for (const auto& r : m_portScanResults) {
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

    html += QStringLiteral("<div class=\"stat-box\">Open: %1 | Closed: %2 | Filtered: %3</div>\n")
                .arg(openCount)
                .arg(closedCount)
                .arg(filteredCount);

    html += QStringLiteral(
        "<table>\n<tr><th>Port</th><th>State</th><th>Service</th>"
        "<th>Response</th><th>Banner</th></tr>\n");

    for (const auto& r : m_portScanResults) {
        if (r.state != PortScanResult::State::Open) {
            continue;  // Only show open ports in report
        }

        html += QStringLiteral(
                    "<tr><td>%1</td><td class=\"success\">Open</td>"
                    "<td>%2</td><td>%3 ms</td><td>%4</td></tr>\n")
                    .arg(r.port)
                    .arg(r.serviceName.toHtmlEscaped())
                    .arg(r.responseTimeMs, 0, 'f', 1)
                    .arg(r.banner.left(80).toHtmlEscaped());
    }
    html += QStringLiteral("</table>\n");
    return html;
}

QString NetworkDiagnosticReportGenerator::buildBandwidthSection() const {
    QString html;
    html += QStringLiteral("<h2>Bandwidth Test Results</h2>\n");

    const auto modeStr = (m_bandwidthResult.mode == BandwidthTestResult::TestMode::LanIperf3)
                             ? QStringLiteral("LAN (iPerf3)")
                             : QStringLiteral("WAN (HTTP)");

    html += QStringLiteral(
                "<div class=\"stat-box\">"
                "Mode: %1 | Target: %2<br>"
                "Download: <b>%3 Mbps</b> | Upload: <b>%4 Mbps</b><br>"
                "Jitter: %5 ms | Packet Loss: %6%%"
                "</div>\n")
                .arg(modeStr, m_bandwidthResult.target.toHtmlEscaped())
                .arg(m_bandwidthResult.downloadMbps, 0, 'f', 2)
                .arg(m_bandwidthResult.uploadMbps, 0, 'f', 2)
                .arg(m_bandwidthResult.jitterMs, 0, 'f', 2)
                .arg(m_bandwidthResult.packetLossPercent, 0, 'f', 2);

    return html;
}

QString NetworkDiagnosticReportGenerator::buildWiFiSection() const {
    QString html;
    html += QStringLiteral("<h2>WiFi Analysis</h2>\n");
    html += QStringLiteral(
        "<table>\n<tr><th>SSID</th><th>BSSID</th><th>Signal</th>"
        "<th>Channel</th><th>Band</th><th>Security</th><th>Vendor</th></tr>\n");

    for (const auto& net : m_wifiNetworks) {
        const auto signalClass = (net.rssiDbm >= -50)   ? QStringLiteral("success")
                                 : (net.rssiDbm >= -70) ? QStringLiteral("warning")
                                                        : QStringLiteral("error");

        const auto connected = net.isConnected ? QStringLiteral(" ★") : QString();

        html += QStringLiteral(
                    "<tr><td>%1%2</td><td>%3</td>"
                    "<td class=\"%4\">%5 dBm (%6%%)</td>"
                    "<td>%7</td><td>%8</td><td>%9</td><td>%10</td></tr>\n")
                    .arg(net.ssid.toHtmlEscaped(), connected, net.bssid, signalClass)
                    .arg(net.rssiDbm)
                    .arg(net.signalQuality)
                    .arg(net.channelNumber)
                    .arg(net.band, net.authentication, net.apVendor);
    }
    html += QStringLiteral("</table>\n");
    return html;
}

QString NetworkDiagnosticReportGenerator::buildFirewallSection() const {
    Q_ASSERT(!m_firewallRules.isEmpty());
    Q_ASSERT(!m_firewallConflicts.isEmpty());
    QString html;
    html += QStringLiteral("<h2>Firewall Audit</h2>\n");

    // Rule summary
    int enabledInbound = 0;
    int enabledOutbound = 0;
    int blockRules = 0;
    for (const auto& rule : m_firewallRules) {
        if (!rule.enabled) {
            continue;
        }
        if (rule.direction == FirewallRule::Direction::Inbound) {
            ++enabledInbound;
        } else {
            ++enabledOutbound;
        }
        if (rule.action == FirewallRule::Action::Block) {
            ++blockRules;
        }
    }

    html += QStringLiteral(
                "<div class=\"stat-box\">"
                "Total Rules: %1 | Inbound: %2 | Outbound: %3 | Block Rules: %4"
                "</div>\n")
                .arg(m_firewallRules.size())
                .arg(enabledInbound)
                .arg(enabledOutbound)
                .arg(blockRules);

    // Conflicts
    if (!m_firewallConflicts.isEmpty()) {
        html += QStringLiteral("<h3 class=\"warning\">Conflicts (%1)</h3>\n<ul>\n")
                    .arg(m_firewallConflicts.size());
        for (const auto& c : m_firewallConflicts) {
            html += QStringLiteral("<li>%1</li>\n").arg(c.conflictDescription.toHtmlEscaped());
        }
        html += QStringLiteral("</ul>\n");
    }

    // Gaps
    if (!m_firewallGaps.isEmpty()) {
        html += QStringLiteral("<h3>Coverage Gaps (%1)</h3>\n<ul>\n").arg(m_firewallGaps.size());
        for (const auto& g : m_firewallGaps) {
            html += QStringLiteral("<li><b>%1</b> — %2</li>\n")
                        .arg(g.description.toHtmlEscaped(), g.recommendation.toHtmlEscaped());
        }
        html += QStringLiteral("</ul>\n");
    }

    return html;
}

QString NetworkDiagnosticReportGenerator::buildConnectionSection() const {
    Q_ASSERT(!m_connections.empty());
    Q_ASSERT(!m_connections.isEmpty());
    QString html;
    html += QStringLiteral("<h2>Active Connections</h2>\n");

    int tcpCount = 0;
    int udpCount = 0;
    int established = 0;
    for (const auto& c : m_connections) {
        if (c.protocol == ConnectionInfo::Protocol::TCP) {
            ++tcpCount;
        } else {
            ++udpCount;
        }
        if (c.state == QStringLiteral("ESTABLISHED")) {
            ++established;
        }
    }

    html += QStringLiteral(
                "<div class=\"stat-box\">"
                "Total: %1 | TCP: %2 | UDP: %3 | Established: %4"
                "</div>\n")
                .arg(m_connections.size())
                .arg(tcpCount)
                .arg(udpCount)
                .arg(established);

    html += QStringLiteral(
        "<table>\n<tr><th>Protocol</th><th>Local</th><th>Remote</th>"
        "<th>State</th><th>Process</th></tr>\n");

    // Only show first 100 to avoid huge reports
    const int limit = qMin(m_connections.size(), 100);
    for (int i = 0; i < limit; ++i) {
        const auto& c = m_connections[i];
        const auto proto = (c.protocol == ConnectionInfo::Protocol::TCP) ? QStringLiteral("TCP")
                                                                         : QStringLiteral("UDP");
        html += QStringLiteral(
                    "<tr><td>%1</td><td>%2:%3</td><td>%4:%5</td>"
                    "<td>%6</td><td>%7</td></tr>\n")
                    .arg(proto, c.localAddress)
                    .arg(c.localPort)
                    .arg(c.remoteAddress)
                    .arg(c.remotePort)
                    .arg(c.state, c.processName.toHtmlEscaped());
    }
    html += QStringLiteral("</table>\n");

    if (m_connections.size() > limit) {
        html += QStringLiteral("<p class=\"meta\">... and %1 more connections</p>\n")
                    .arg(m_connections.size() - limit);
    }

    return html;
}

QString NetworkDiagnosticReportGenerator::buildShareSection() const {
    QString html;
    html += QStringLiteral("<h2>Network Shares</h2>\n");
    html += QStringLiteral(
        "<table>\n<tr><th>UNC Path</th><th>Type</th>"
        "<th>Read</th><th>Write</th><th>Remark</th></tr>\n");

    for (const auto& s : m_shares) {
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

        const auto readIcon = s.canRead ? QStringLiteral("✓") : QStringLiteral("✗");
        const auto writeIcon = s.canWrite ? QStringLiteral("✓") : QStringLiteral("✗");
        const auto readClass = s.canRead ? QStringLiteral("success") : QStringLiteral("error");
        const auto writeClass = s.canWrite ? QStringLiteral("success") : QStringLiteral("error");

        html += QStringLiteral(
                    "<tr><td>%1</td><td>%2</td>"
                    "<td class=\"%3\">%4</td><td class=\"%5\">%6</td>"
                    "<td>%7</td></tr>\n")
                    .arg(s.uncPath.toHtmlEscaped(),
                         typeStr,
                         readClass,
                         readIcon,
                         writeClass,
                         writeIcon,
                         s.remark.toHtmlEscaped());
    }
    html += QStringLiteral("</table>\n");
    return html;
}

QString NetworkDiagnosticReportGenerator::buildHtmlFooter() const {
    return QStringLiteral(
        "<hr>\n"
        "<p class=\"meta\">Generated by S.A.K. Utility — "
        "Network Diagnostics & Troubleshooting</p>\n"
        "</body>\n</html>\n");
}

void NetworkDiagnosticReportGenerator::populateRootJson(QJsonObject& root) const {
    Q_ASSERT(!root.isEmpty());
    Q_ASSERT(!m_sections.isEmpty());
    root[QStringLiteral("reportType")] = QStringLiteral("NetworkDiagnostic");
    root[QStringLiteral("generated")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("technician")] = m_technicianName;
    root[QStringLiteral("ticket")] = m_ticketNumber;
    root[QStringLiteral("notes")] = m_notes;

    appendJsonSections(root);
}

void NetworkDiagnosticReportGenerator::appendHtmlSections(QString& html) const {
    using Builder = QString (NetworkDiagnosticReportGenerator::*)() const;
    struct SectionBuilder {
        Section section;
        Builder builder;
    };
    static const SectionBuilder kBuilders[] = {
        {Section::AdapterConfig, &NetworkDiagnosticReportGenerator::buildAdapterSection},
        {Section::PingResults, &NetworkDiagnosticReportGenerator::buildPingSection},
        {Section::TracerouteResults, &NetworkDiagnosticReportGenerator::buildTracerouteSection},
        {Section::DnsResults, &NetworkDiagnosticReportGenerator::buildDnsSection},
        {Section::PortScanResults, &NetworkDiagnosticReportGenerator::buildPortScanSection},
        {Section::BandwidthResults, &NetworkDiagnosticReportGenerator::buildBandwidthSection},
        {Section::WiFiAnalysis, &NetworkDiagnosticReportGenerator::buildWiFiSection},
        {Section::FirewallAudit, &NetworkDiagnosticReportGenerator::buildFirewallSection},
        {Section::ActiveConnections, &NetworkDiagnosticReportGenerator::buildConnectionSection},
        {Section::NetworkShares, &NetworkDiagnosticReportGenerator::buildShareSection},
    };

    for (const auto& entry : kBuilders) {
        if (m_sections.contains(entry.section)) {
            html += (this->*entry.builder)();
        }
    }
}

void NetworkDiagnosticReportGenerator::appendJsonSections(QJsonObject& root) const {
    using Appender = void (NetworkDiagnosticReportGenerator::*)(QJsonObject&) const;
    struct SectionAppender {
        Section section;
        Appender appender;
    };
    static const SectionAppender kAppenders[] = {
        {Section::AdapterConfig, &NetworkDiagnosticReportGenerator::appendAdapterConfigJson},
        {Section::PingResults, &NetworkDiagnosticReportGenerator::appendPingResultsJson},
        {Section::TracerouteResults,
         &NetworkDiagnosticReportGenerator::appendTracerouteResultsJson},
        {Section::DnsResults, &NetworkDiagnosticReportGenerator::appendDnsResultsJson},
        {Section::PortScanResults, &NetworkDiagnosticReportGenerator::appendPortScanResultsJson},
        {Section::BandwidthResults, &NetworkDiagnosticReportGenerator::appendBandwidthResultsJson},
        {Section::WiFiAnalysis, &NetworkDiagnosticReportGenerator::appendWiFiAnalysisJson},
        {Section::FirewallAudit, &NetworkDiagnosticReportGenerator::appendFirewallAuditJson},
        {Section::ActiveConnections,
         &NetworkDiagnosticReportGenerator::appendActiveConnectionsJson},
        {Section::NetworkShares, &NetworkDiagnosticReportGenerator::appendNetworkSharesJson},
    };

    for (const auto& entry : kAppenders) {
        if (m_sections.contains(entry.section)) {
            (this->*entry.appender)(root);
        }
    }
}

void NetworkDiagnosticReportGenerator::appendAdapterConfigJson(QJsonObject& root) const {
    Q_ASSERT(!root.isEmpty());
    QJsonArray adapters;
    for (const auto& a : m_adapters) {
        QJsonObject obj;
        obj[QStringLiteral("name")] = a.name;
        obj[QStringLiteral("type")] = a.adapterType;
        obj[QStringLiteral("mac")] = a.macAddress;
        obj[QStringLiteral("connected")] = a.isConnected;
        obj[QStringLiteral("speedBps")] = static_cast<qint64>(a.linkSpeedBps);

        QJsonArray ips;
        for (const auto& ip : a.ipv4Addresses) {
            ips.append(ip);
        }
        obj[QStringLiteral("ipv4")] = ips;
        adapters.append(obj);
    }
    root[QStringLiteral("adapters")] = adapters;
}

void NetworkDiagnosticReportGenerator::appendPingResultsJson(QJsonObject& root) const {
    QJsonObject ping;
    ping[QStringLiteral("target")] = m_pingResult.target;
    ping[QStringLiteral("sent")] = m_pingResult.sent;
    ping[QStringLiteral("received")] = m_pingResult.received;
    ping[QStringLiteral("lossPercent")] = m_pingResult.lossPercent;
    ping[QStringLiteral("minRtt")] = m_pingResult.minRtt;
    ping[QStringLiteral("maxRtt")] = m_pingResult.maxRtt;
    ping[QStringLiteral("avgRtt")] = m_pingResult.avgRtt;
    ping[QStringLiteral("jitter")] = m_pingResult.jitter;
    root[QStringLiteral("ping")] = ping;
}

void NetworkDiagnosticReportGenerator::appendTracerouteResultsJson(QJsonObject& root) const {
    Q_ASSERT(!root.isEmpty());
    QJsonObject trace;
    trace[QStringLiteral("target")] = m_tracerouteResult.target;
    trace[QStringLiteral("reached")] = m_tracerouteResult.reachedTarget;

    QJsonArray hops;
    for (const auto& h : m_tracerouteResult.hops) {
        QJsonObject hop;
        hop[QStringLiteral("hop")] = h.hopNumber;
        hop[QStringLiteral("ip")] = h.ipAddress;
        hop[QStringLiteral("hostname")] = h.hostname;
        hop[QStringLiteral("avgRtt")] = h.avgRttMs;
        hop[QStringLiteral("timedOut")] = h.timedOut;
        hops.append(hop);
    }
    trace[QStringLiteral("hops")] = hops;
    root[QStringLiteral("traceroute")] = trace;
}

void NetworkDiagnosticReportGenerator::appendDnsResultsJson(QJsonObject& root) const {
    Q_ASSERT(!root.isEmpty());
    QJsonArray dnsArr;
    for (const auto& r : m_dnsResults) {
        QJsonObject obj;
        obj[QStringLiteral("query")] = r.queryName;
        obj[QStringLiteral("type")] = r.recordType;
        obj[QStringLiteral("server")] = r.dnsServer;
        obj[QStringLiteral("success")] = r.success;
        obj[QStringLiteral("responseMs")] = r.responseTimeMs;

        QJsonArray answers;
        for (const auto& a : r.answers) {
            answers.append(a);
        }
        obj[QStringLiteral("answers")] = answers;
        dnsArr.append(obj);
    }
    root[QStringLiteral("dns")] = dnsArr;
}

void NetworkDiagnosticReportGenerator::appendPortScanResultsJson(QJsonObject& root) const {
    Q_ASSERT(!root.isEmpty());
    QJsonArray portArr;
    for (const auto& r : m_portScanResults) {
        QJsonObject obj;
        obj[QStringLiteral("port")] = r.port;
        obj[QStringLiteral("state")] =
            (r.state == PortScanResult::State::Open)       ? QStringLiteral("open")
            : (r.state == PortScanResult::State::Closed)   ? QStringLiteral("closed")
            : (r.state == PortScanResult::State::Filtered) ? QStringLiteral("filtered")
                                                           : QStringLiteral("error");
        obj[QStringLiteral("service")] = r.serviceName;
        obj[QStringLiteral("responseMs")] = r.responseTimeMs;
        portArr.append(obj);
    }
    root[QStringLiteral("portScan")] = portArr;
}

void NetworkDiagnosticReportGenerator::appendBandwidthResultsJson(QJsonObject& root) const {
    QJsonObject bw;
    bw[QStringLiteral("downloadMbps")] = m_bandwidthResult.downloadMbps;
    bw[QStringLiteral("uploadMbps")] = m_bandwidthResult.uploadMbps;
    bw[QStringLiteral("jitterMs")] = m_bandwidthResult.jitterMs;
    bw[QStringLiteral("packetLoss")] = m_bandwidthResult.packetLossPercent;
    root[QStringLiteral("bandwidth")] = bw;
}

void NetworkDiagnosticReportGenerator::appendWiFiAnalysisJson(QJsonObject& root) const {
    QJsonArray wifiArr;
    for (const auto& n : m_wifiNetworks) {
        QJsonObject obj;
        obj[QStringLiteral("ssid")] = n.ssid;
        obj[QStringLiteral("bssid")] = n.bssid;
        obj[QStringLiteral("rssi")] = n.rssiDbm;
        obj[QStringLiteral("channel")] = n.channelNumber;
        obj[QStringLiteral("band")] = n.band;
        obj[QStringLiteral("security")] = n.authentication;
        obj[QStringLiteral("vendor")] = n.apVendor;
        wifiArr.append(obj);
    }
    root[QStringLiteral("wifi")] = wifiArr;
}

void NetworkDiagnosticReportGenerator::appendFirewallAuditJson(QJsonObject& root) const {
    QJsonObject fw;
    fw[QStringLiteral("totalRules")] = m_firewallRules.size();
    fw[QStringLiteral("conflicts")] = m_firewallConflicts.size();
    fw[QStringLiteral("gaps")] = m_firewallGaps.size();
    root[QStringLiteral("firewall")] = fw;
}

void NetworkDiagnosticReportGenerator::appendActiveConnectionsJson(QJsonObject& root) const {
    QJsonObject conn;
    conn[QStringLiteral("total")] = m_connections.size();
    root[QStringLiteral("connections")] = conn;
}

void NetworkDiagnosticReportGenerator::appendNetworkSharesJson(QJsonObject& root) const {
    QJsonArray shareArr;
    for (const auto& s : m_shares) {
        QJsonObject obj;
        obj[QStringLiteral("path")] = s.uncPath;
        obj[QStringLiteral("canRead")] = s.canRead;
        obj[QStringLiteral("canWrite")] = s.canWrite;
        shareArr.append(obj);
    }
    root[QStringLiteral("shares")] = shareArr;
}

QString NetworkDiagnosticReportGenerator::toJson() const {
    QJsonObject root;
    populateRootJson(root);

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

}  // namespace sak
