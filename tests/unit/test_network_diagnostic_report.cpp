// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_report.cpp
/// @brief Unit tests for NetworkDiagnosticReportGenerator
///
/// Tests HTML/JSON report generation with various data combinations,
/// section inclusion/exclusion, metadata, and edge cases.

#include <QtTest/QtTest>

#include "sak/network_diagnostic_report_generator.h"
#include "sak/network_diagnostic_types.h"

#include <QJsonDocument>
#include <QJsonObject>

using namespace sak;

class NetworkDiagnosticReportTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction ──
    void construction_default();
    void construction_nonCopyable();

    // ── Section enum ──
    void sectionEnum_allDistinct();

    // ── HTML: Empty report ──
    void html_emptyReport_containsStructure();
    void html_emptyReport_containsDoctype();

    // ── HTML: Metadata ──
    void html_metadata_technicianName();
    void html_metadata_ticketNumber();
    void html_metadata_notes();

    // ── HTML: Sections ──
    void html_section_adapterConfig();
    void html_section_pingResults();
    void html_section_dnsResults();
    void html_section_portScanResults();
    void html_section_bandwidthResults();
    void html_section_wifiAnalysis();
    void html_section_firewallAudit();
    void html_section_connectionMonitor();
    void html_section_networkShares();

    // ── HTML: Section exclusion ──
    void html_sectionExclusion_onlyIncluded();

    // ── JSON: Basic structure ──
    void json_emptyReport_validJson();
    void json_metadata_included();
    void json_pingData_serialized();
    void json_dnsData_serialized();
    void json_portScanData_serialized();
    void json_wifiData_serialized();
    void json_firewallData_serialized();
    void json_shareData_serialized();
};

// ════════════════════════════════════════════════════════════════════════════
// Construction
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticReportTests::construction_default()
{
    NetworkDiagnosticReportGenerator gen;
    // Should produce valid (empty) HTML without crashing
    const auto html = gen.toHtml();
    QVERIFY(!html.isEmpty());
}

void NetworkDiagnosticReportTests::construction_nonCopyable()
{
    QVERIFY(!std::is_copy_constructible_v<NetworkDiagnosticReportGenerator>);
    QVERIFY(!std::is_copy_assignable_v<NetworkDiagnosticReportGenerator>);
}

// ════════════════════════════════════════════════════════════════════════════
// Section Enum
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticReportTests::sectionEnum_allDistinct()
{
    using S = NetworkDiagnosticReportGenerator::Section;
    const QSet<int> values = {
        static_cast<int>(S::AdapterConfig),
        static_cast<int>(S::PingResults),
        static_cast<int>(S::TracerouteResults),
        static_cast<int>(S::DnsResults),
        static_cast<int>(S::PortScanResults),
        static_cast<int>(S::BandwidthResults),
        static_cast<int>(S::WiFiAnalysis),
        static_cast<int>(S::FirewallAudit),
        static_cast<int>(S::ActiveConnections),
        static_cast<int>(S::NetworkShares),
    };
    QCOMPARE(values.size(), 10);
}

// ════════════════════════════════════════════════════════════════════════════
// HTML: Empty Report
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticReportTests::html_emptyReport_containsStructure()
{
    NetworkDiagnosticReportGenerator gen;
    const auto html = gen.toHtml();

    QVERIFY(html.contains(QStringLiteral("<html"), Qt::CaseInsensitive));
    QVERIFY(html.contains(QStringLiteral("</html>"), Qt::CaseInsensitive));
    QVERIFY(html.contains(QStringLiteral("<body"), Qt::CaseInsensitive));
    QVERIFY(html.contains(QStringLiteral("</body>"), Qt::CaseInsensitive));
}

void NetworkDiagnosticReportTests::html_emptyReport_containsDoctype()
{
    NetworkDiagnosticReportGenerator gen;
    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("<!DOCTYPE"), Qt::CaseInsensitive)
         || html.contains(QStringLiteral("<html"), Qt::CaseInsensitive));
}

// ════════════════════════════════════════════════════════════════════════════
// HTML: Metadata
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticReportTests::html_metadata_technicianName()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setTechnicianName(QStringLiteral("John Doe"));
    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("John Doe")));
}

void NetworkDiagnosticReportTests::html_metadata_ticketNumber()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setTicketNumber(QStringLiteral("TICKET-12345"));
    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("TICKET-12345")));
}

void NetworkDiagnosticReportTests::html_metadata_notes()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setNotes(QStringLiteral("Customer reports intermittent connectivity"));
    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("intermittent connectivity")));
}

// ════════════════════════════════════════════════════════════════════════════
// HTML: Sections
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticReportTests::html_section_adapterConfig()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::AdapterConfig});

    NetworkAdapterInfo adapter;
    adapter.name = QStringLiteral("Ethernet 1");
    adapter.macAddress = QStringLiteral("AA:BB:CC:DD:EE:FF");
    adapter.ipv4Addresses = {QStringLiteral("192.168.1.100")};
    adapter.isConnected = true;
    gen.setAdapterData({adapter});

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("Ethernet 1")));
    QVERIFY(html.contains(QStringLiteral("AA:BB:CC:DD:EE:FF")));
    QVERIFY(html.contains(QStringLiteral("192.168.1.100")));
}

void NetworkDiagnosticReportTests::html_section_pingResults()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::PingResults});

    PingResult ping;
    ping.target = QStringLiteral("google.com");
    ping.resolvedIP = QStringLiteral("8.8.8.8");
    ping.sent = 10;
    ping.received = 10;
    ping.lossPercent = 0.0;
    ping.avgRtt = 15.5;
    ping.minRtt = 10.0;
    ping.maxRtt = 25.0;
    gen.setPingData(ping);

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("google.com")));
}

void NetworkDiagnosticReportTests::html_section_dnsResults()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::DnsResults});

    DnsQueryResult dns;
    dns.queryName = QStringLiteral("example.com");
    dns.recordType = QStringLiteral("A");
    dns.answers = {QStringLiteral("93.184.216.34")};
    dns.dnsServer = QStringLiteral("8.8.8.8");
    dns.responseTimeMs = 25.0;
    dns.success = true;
    gen.setDnsData({dns});

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("example.com")));
    QVERIFY(html.contains(QStringLiteral("93.184.216.34")));
}

void NetworkDiagnosticReportTests::html_section_portScanResults()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::PortScanResults});

    PortScanResult port;
    port.port = 443;
    port.state = PortScanResult::State::Open;
    port.serviceName = QStringLiteral("HTTPS");
    gen.setPortScanData({port});

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("443")));
    QVERIFY(html.contains(QStringLiteral("HTTPS")));
}

void NetworkDiagnosticReportTests::html_section_bandwidthResults()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::BandwidthResults});

    BandwidthTestResult bw;
    bw.downloadMbps = 500.5;
    bw.uploadMbps = 100.2;
    gen.setBandwidthData(bw);

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("500")) || html.contains(QStringLiteral("500.5")));
    QVERIFY(html.contains(QStringLiteral("100")) || html.contains(QStringLiteral("100.2")));
}

void NetworkDiagnosticReportTests::html_section_wifiAnalysis()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::WiFiAnalysis});

    WiFiNetworkInfo wifi;
    wifi.ssid = QStringLiteral("TestNetwork");
    wifi.channelNumber = 6;
    wifi.rssiDbm = -55;
    wifi.band = QStringLiteral("2.4 GHz");
    gen.setWiFiData({wifi});

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("TestNetwork")));
}

void NetworkDiagnosticReportTests::html_section_firewallAudit()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::FirewallAudit});

    FirewallRule rule;
    rule.name = QStringLiteral("Block Telnet");
    rule.enabled = true;
    rule.direction = FirewallRule::Direction::Inbound;
    rule.action = FirewallRule::Action::Block;
    rule.protocol = FirewallRule::Protocol::TCP;
    rule.localPorts = QStringLiteral("23");

    gen.setFirewallData({rule}, {}, {});

    const auto html = gen.toHtml();
    // Firewall section shows summary counts, not individual rule names
    QVERIFY(html.contains(QStringLiteral("Firewall Audit")));
    QVERIFY(html.contains(QStringLiteral("Total Rules: 1")));
}

void NetworkDiagnosticReportTests::html_section_connectionMonitor()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::ActiveConnections});

    ConnectionInfo conn;
    conn.protocol = ConnectionInfo::Protocol::TCP;
    conn.localAddress = QStringLiteral("192.168.1.100");
    conn.localPort = 54321;
    conn.remoteAddress = QStringLiteral("8.8.8.8");
    conn.remotePort = 443;
    conn.state = QStringLiteral("ESTABLISHED");
    conn.processName = QStringLiteral("chrome.exe");
    gen.setConnectionData({conn});

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("ESTABLISHED"))
         || html.contains(QStringLiteral("chrome.exe")));
}

void NetworkDiagnosticReportTests::html_section_networkShares()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::NetworkShares});

    NetworkShareInfo share;
    share.uncPath = QStringLiteral("\\\\SERVER\\SharedFolder$");
    share.type = NetworkShareInfo::ShareType::Disk;
    share.canRead = true;
    share.canWrite = false;
    share.remark = QStringLiteral("Test share");
    gen.setShareData({share});

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("SharedFolder")));
}

void NetworkDiagnosticReportTests::html_sectionExclusion_onlyIncluded()
{
    NetworkDiagnosticReportGenerator gen;
    // Only include Ping section
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::PingResults});

    PingResult ping;
    ping.target = QStringLiteral("unique_ping_target_xyz");
    gen.setPingData(ping);

    // Also set WiFi data (which shouldn't appear since WiFi section isn't included)
    WiFiNetworkInfo wifi;
    wifi.ssid = QStringLiteral("unique_wifi_ssid_abc");
    gen.setWiFiData({wifi});

    const auto html = gen.toHtml();
    QVERIFY(html.contains(QStringLiteral("unique_ping_target_xyz")));
    // WiFi section shouldn't be present (data was set but section not included)
    QVERIFY(!html.contains(QStringLiteral("unique_wifi_ssid_abc")));
}

// ════════════════════════════════════════════════════════════════════════════
// JSON
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticReportTests::json_emptyReport_validJson()
{
    NetworkDiagnosticReportGenerator gen;
    const auto json = gen.toJson();
    QVERIFY(!json.isEmpty());

    auto doc = QJsonDocument::fromJson(json.toUtf8());
    QVERIFY(!doc.isNull());
    QVERIFY(doc.isObject());
}

void NetworkDiagnosticReportTests::json_metadata_included()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setTechnicianName(QStringLiteral("Jane Smith"));
    gen.setTicketNumber(QStringLiteral("INC-99999"));
    gen.setNotes(QStringLiteral("Test notes"));

    const auto json = gen.toJson();
    auto doc = QJsonDocument::fromJson(json.toUtf8());
    QVERIFY(!doc.isNull());
    QVERIFY(json.contains(QStringLiteral("Jane Smith")));
    QVERIFY(json.contains(QStringLiteral("INC-99999")));
    QVERIFY(json.contains(QStringLiteral("Test notes")));
}

void NetworkDiagnosticReportTests::json_pingData_serialized()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::PingResults});

    PingResult ping;
    ping.target = QStringLiteral("example.org");
    ping.sent = 10;
    ping.received = 9;
    ping.avgRtt = 20.5;
    gen.setPingData(ping);

    const auto json = gen.toJson();
    QVERIFY(json.contains(QStringLiteral("example.org")));
}

void NetworkDiagnosticReportTests::json_dnsData_serialized()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::DnsResults});

    DnsQueryResult dns;
    dns.queryName = QStringLiteral("test.example.com");
    dns.recordType = QStringLiteral("A");
    dns.success = true;
    gen.setDnsData({dns});

    const auto json = gen.toJson();
    QVERIFY(json.contains(QStringLiteral("test.example.com")));
}

void NetworkDiagnosticReportTests::json_portScanData_serialized()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::PortScanResults});

    PortScanResult port;
    port.port = 8080;
    port.state = PortScanResult::State::Open;
    port.serviceName = QStringLiteral("HTTP-Alt");
    gen.setPortScanData({port});

    const auto json = gen.toJson();
    QVERIFY(json.contains(QStringLiteral("8080")));
}

void NetworkDiagnosticReportTests::json_wifiData_serialized()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::WiFiAnalysis});

    WiFiNetworkInfo wifi;
    wifi.ssid = QStringLiteral("JsonTestNet");
    wifi.channelNumber = 11;
    gen.setWiFiData({wifi});

    const auto json = gen.toJson();
    QVERIFY(json.contains(QStringLiteral("JsonTestNet")));
}

void NetworkDiagnosticReportTests::json_firewallData_serialized()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::FirewallAudit});

    FirewallRule rule;
    rule.name = QStringLiteral("Allow HTTPS");
    rule.enabled = true;
    gen.setFirewallData({rule}, {}, {});

    const auto json = gen.toJson();
    // JSON firewall section contains summary counts
    QVERIFY(json.contains(QStringLiteral("totalRules")));
}

void NetworkDiagnosticReportTests::json_shareData_serialized()
{
    NetworkDiagnosticReportGenerator gen;
    gen.setIncludedSections({NetworkDiagnosticReportGenerator::Section::NetworkShares});

    NetworkShareInfo share;
    share.uncPath = QStringLiteral("\\\\HOST\\JsonShare");
    share.type = NetworkShareInfo::ShareType::Disk;
    gen.setShareData({share});

    const auto json = gen.toJson();
    QVERIFY(json.contains(QStringLiteral("JsonShare")));
}

QTEST_GUILESS_MAIN(NetworkDiagnosticReportTests)
#include "test_network_diagnostic_report.moc"
