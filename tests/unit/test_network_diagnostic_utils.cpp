// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_utils.cpp
/// @brief Unit tests for Network Diagnostic utility / static methods
///
/// Tests WiFi frequency conversion, vendor lookup, channel utilization,
/// port scanner presets and service names, and DNS server/record type lists.

#include <QtTest/QtTest>

#include "sak/wifi_analyzer.h"
#include "sak/port_scanner.h"
#include "sak/dns_diagnostic_tool.h"
#include "sak/network_diagnostic_types.h"

#include <algorithm>

using namespace sak;

class NetworkDiagnosticUtilsTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── WiFi: frequencyToChannel ──
    void wifi_freqToChannel_2_4GHz_channel1();
    void wifi_freqToChannel_2_4GHz_channel6();
    void wifi_freqToChannel_2_4GHz_channel11();
    void wifi_freqToChannel_2_4GHz_channel13();
    void wifi_freqToChannel_2_4GHz_channel14();
    void wifi_freqToChannel_5GHz_channel36();
    void wifi_freqToChannel_5GHz_channel44();
    void wifi_freqToChannel_5GHz_channel149();
    void wifi_freqToChannel_6GHz_channel1();
    void wifi_freqToChannel_6GHz_channel5();
    void wifi_freqToChannel_unknown();
    void wifi_freqToChannel_zero();

    // ── WiFi: frequencyToBand ──
    void wifi_freqToBand_2_4GHz();
    void wifi_freqToBand_5GHz();
    void wifi_freqToBand_6GHz();
    void wifi_freqToBand_unknown();
    void wifi_freqToBand_boundaries();

    // ── WiFi: lookupVendor ──
    void wifi_lookupVendor_knownOui();
    void wifi_lookupVendor_unknownOui();
    void wifi_lookupVendor_tooShort();
    void wifi_lookupVendor_empty();
    void wifi_lookupVendor_caseInsensitive();

    // ── WiFi: calculateChannelUtilization ──
    void wifi_channelUtil_emptyInput();
    void wifi_channelUtil_singleNetwork();
    void wifi_channelUtil_multipleOnSameChannel();
    void wifi_channelUtil_multipleChannels();
    void wifi_channelUtil_sortedByChannel();
    void wifi_channelUtil_averageSignal();
    void wifi_channelUtil_interferenceNonNegative();

    // ── PortScanner: getPresets ──
    void port_presets_notEmpty();
    void port_presets_haveNames();
    void port_presets_havePorts();
    void port_presets_commonServices();
    void port_presets_top100HasExpectedPorts();

    // ── PortScanner: getServiceName ──
    void port_serviceName_http();
    void port_serviceName_https();
    void port_serviceName_ssh();
    void port_serviceName_ftp();
    void port_serviceName_dns();
    void port_serviceName_rdp();
    void port_serviceName_unknown();

    // ── DNS: wellKnownDnsServers ──
    void dns_servers_notEmpty();
    void dns_servers_haveNames();
    void dns_servers_includeGoogle();
    void dns_servers_includeCloudflare();
    void dns_servers_includeSystemDefault();

    // ── DNS: supportedRecordTypes ──
    void dns_recordTypes_notEmpty();
    void dns_recordTypes_includeCommon();
    void dns_recordTypes_includeAll();
};

// ════════════════════════════════════════════════════════════════════════════
// WiFi: frequencyToChannel
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel1()
{
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2412000), 1);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel6()
{
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2437000), 6);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel11()
{
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2462000), 11);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel13()
{
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2472000), 13);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel14()
{
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2484000), 14);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_5GHz_channel36()
{
    // 5180 MHz = channel 36
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5180000), 36);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_5GHz_channel44()
{
    // 5220 MHz = channel 44
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5220000), 44);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_5GHz_channel149()
{
    // 5745 MHz = channel 149
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5745000), 149);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_6GHz_channel1()
{
    // 5955 MHz = 6 GHz channel 1
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5955000), 1);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_6GHz_channel5()
{
    // 5975 MHz = 6 GHz channel 5
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5975000), 5);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_unknown()
{
    // Out-of-band frequency
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(900000), 0);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_zero()
{
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(0), 0);
}

// ════════════════════════════════════════════════════════════════════════════
// WiFi: frequencyToBand
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::wifi_freqToBand_2_4GHz()
{
    QCOMPARE(WiFiAnalyzer::frequencyToBand(2412000), QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(2437000), QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(2484000), QStringLiteral("2.4 GHz"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_5GHz()
{
    QCOMPARE(WiFiAnalyzer::frequencyToBand(5180000), QStringLiteral("5 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(5745000), QStringLiteral("5 GHz"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_6GHz()
{
    QCOMPARE(WiFiAnalyzer::frequencyToBand(5955000), QStringLiteral("6 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(7000000), QStringLiteral("6 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(7115000), QStringLiteral("6 GHz"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_unknown()
{
    QCOMPARE(WiFiAnalyzer::frequencyToBand(900000), QStringLiteral("Unknown"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(0), QStringLiteral("Unknown"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_boundaries()
{
    // Start boundaries
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq2GHzStart),
             QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq5GHzStart),
             QStringLiteral("5 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq6GHzStart),
             QStringLiteral("6 GHz"));

    // End boundaries
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq2GHzEnd),
             QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq5GHzEnd),
             QStringLiteral("5 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq6GHzEnd),
             QStringLiteral("6 GHz"));
}

// ════════════════════════════════════════════════════════════════════════════
// WiFi: lookupVendor
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_knownOui()
{
    // The OUI database should contain major manufacturers
    // We can verify that *some* well-known OUI returns a non-empty string
    // without depending on the exact vendor name
    const auto result = WiFiAnalyzer::lookupVendor(QStringLiteral("AA:BB:CC:DD:EE:FF"));
    // May or may not be in the database — just test it doesn't crash
    Q_UNUSED(result);
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_unknownOui()
{
    const auto result = WiFiAnalyzer::lookupVendor(QStringLiteral("00:00:00:00:00:00"));
    // Unknown OUI should return empty or a valid string (no crash)
    Q_UNUSED(result);
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_tooShort()
{
    const auto result = WiFiAnalyzer::lookupVendor(QStringLiteral("AA:BB"));
    QVERIFY(result.isEmpty());
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_empty()
{
    const auto result = WiFiAnalyzer::lookupVendor(QString());
    QVERIFY(result.isEmpty());
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_caseInsensitive()
{
    // Both upper and lower should produce the same result
    const auto upper = WiFiAnalyzer::lookupVendor(QStringLiteral("AA:BB:CC:DD:EE:FF"));
    const auto lower = WiFiAnalyzer::lookupVendor(QStringLiteral("aa:bb:cc:dd:ee:ff"));
    QCOMPARE(upper, lower);
}

// ════════════════════════════════════════════════════════════════════════════
// WiFi: calculateChannelUtilization
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::wifi_channelUtil_emptyInput()
{
    QVector<WiFiNetworkInfo> empty;
    const auto result = WiFiAnalyzer::calculateChannelUtilization(empty);
    QVERIFY(result.isEmpty());
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_singleNetwork()
{
    QVector<WiFiNetworkInfo> networks;
    WiFiNetworkInfo net;
    net.ssid = QStringLiteral("TestNet");
    net.channelNumber = 6;
    net.band = QStringLiteral("2.4 GHz");
    net.rssiDbm = -50;
    networks.append(net);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result[0].channelNumber, 6);
    QCOMPARE(result[0].networkCount, 1);
    QCOMPARE(result[0].ssids.size(), 1);
    QCOMPARE(result[0].ssids[0], QStringLiteral("TestNet"));
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_multipleOnSameChannel()
{
    QVector<WiFiNetworkInfo> networks;

    WiFiNetworkInfo net1;
    net1.ssid = QStringLiteral("Net1");
    net1.channelNumber = 1;
    net1.band = QStringLiteral("2.4 GHz");
    net1.rssiDbm = -50;
    networks.append(net1);

    WiFiNetworkInfo net2;
    net2.ssid = QStringLiteral("Net2");
    net2.channelNumber = 1;
    net2.band = QStringLiteral("2.4 GHz");
    net2.rssiDbm = -70;
    networks.append(net2);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result[0].channelNumber, 1);
    QCOMPARE(result[0].networkCount, 2);
    QCOMPARE(result[0].ssids.size(), 2);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_multipleChannels()
{
    QVector<WiFiNetworkInfo> networks;

    WiFiNetworkInfo net1;
    net1.ssid = QStringLiteral("Net1");
    net1.channelNumber = 1;
    net1.band = QStringLiteral("2.4 GHz");
    net1.rssiDbm = -50;
    networks.append(net1);

    WiFiNetworkInfo net2;
    net2.ssid = QStringLiteral("Net2");
    net2.channelNumber = 6;
    net2.band = QStringLiteral("2.4 GHz");
    net2.rssiDbm = -65;
    networks.append(net2);

    WiFiNetworkInfo net3;
    net3.ssid = QStringLiteral("Net3");
    net3.channelNumber = 11;
    net3.band = QStringLiteral("2.4 GHz");
    net3.rssiDbm = -75;
    networks.append(net3);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    QCOMPARE(result.size(), 3);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_sortedByChannel()
{
    QVector<WiFiNetworkInfo> networks;

    // Add networks in reverse channel order
    WiFiNetworkInfo net1;
    net1.channelNumber = 11;
    net1.rssiDbm = -50;
    networks.append(net1);

    WiFiNetworkInfo net2;
    net2.channelNumber = 1;
    net2.rssiDbm = -60;
    networks.append(net2);

    WiFiNetworkInfo net3;
    net3.channelNumber = 6;
    net3.rssiDbm = -55;
    networks.append(net3);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    QCOMPARE(result.size(), 3);

    // Must be sorted by channel number
    QCOMPARE(result[0].channelNumber, 1);
    QCOMPARE(result[1].channelNumber, 6);
    QCOMPARE(result[2].channelNumber, 11);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_averageSignal()
{
    QVector<WiFiNetworkInfo> networks;

    WiFiNetworkInfo net1;
    net1.channelNumber = 1;
    net1.rssiDbm = -50;
    networks.append(net1);

    WiFiNetworkInfo net2;
    net2.channelNumber = 1;
    net2.rssiDbm = -70;
    networks.append(net2);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    QCOMPARE(result.size(), 1);
    // Average of -50 and -70 = -60
    QCOMPARE(result[0].averageSignalDbm, -60.0);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_interferenceNonNegative()
{
    QVector<WiFiNetworkInfo> networks;

    // Very weak signals (weaker than kSignalWeak = -80)
    WiFiNetworkInfo net;
    net.channelNumber = 1;
    net.rssiDbm = -90;
    networks.append(net);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    QCOMPARE(result.size(), 1);
    QVERIFY(result[0].interferenceScore >= 0.0);
}

// ════════════════════════════════════════════════════════════════════════════
// PortScanner: getPresets
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::port_presets_notEmpty()
{
    const auto presets = PortScanner::getPresets();
    QVERIFY(!presets.isEmpty());
    QVERIFY(presets.size() >= 5);  // At least we expect several presets
}

void NetworkDiagnosticUtilsTests::port_presets_haveNames()
{
    const auto presets = PortScanner::getPresets();
    for (const auto& p : presets) {
        QVERIFY2(!p.name.isEmpty(),
                 qPrintable(QStringLiteral("Preset has empty name")));
    }
}

void NetworkDiagnosticUtilsTests::port_presets_havePorts()
{
    const auto presets = PortScanner::getPresets();
    for (const auto& p : presets) {
        QVERIFY2(!p.ports.isEmpty(),
                 qPrintable(QStringLiteral("Preset '%1' has no ports").arg(p.name)));
    }
}

void NetworkDiagnosticUtilsTests::port_presets_commonServices()
{
    const auto presets = PortScanner::getPresets();

    // Find "Common Services" preset
    bool found = false;
    for (const auto& p : presets) {
        if (p.name.contains(QStringLiteral("Common"), Qt::CaseInsensitive)) {
            found = true;
            QVERIFY(p.ports.contains(80));   // HTTP
            QVERIFY(p.ports.contains(443));  // HTTPS
            QVERIFY(p.ports.contains(22));   // SSH
            QVERIFY(p.ports.contains(53));   // DNS
            break;
        }
    }
    QVERIFY2(found, "Could not find 'Common Services' preset");
}

void NetworkDiagnosticUtilsTests::port_presets_top100HasExpectedPorts()
{
    const auto presets = PortScanner::getPresets();

    bool found = false;
    for (const auto& p : presets) {
        if (p.name.contains(QStringLiteral("Top 100"), Qt::CaseInsensitive)) {
            found = true;
            QVERIFY(p.ports.size() >= 90);  // Roughly 100 ports
            QVERIFY(p.ports.contains(80));
            QVERIFY(p.ports.contains(443));
            QVERIFY(p.ports.contains(22));
            QVERIFY(p.ports.contains(3389)); // RDP
            break;
        }
    }
    QVERIFY2(found, "Could not find 'Top 100' preset");
}

// ════════════════════════════════════════════════════════════════════════════
// PortScanner: getServiceName
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::port_serviceName_http()
{
    const auto name = PortScanner::getServiceName(80);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("HTTP"), Qt::CaseInsensitive));
}

void NetworkDiagnosticUtilsTests::port_serviceName_https()
{
    const auto name = PortScanner::getServiceName(443);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("HTTPS"), Qt::CaseInsensitive));
}

void NetworkDiagnosticUtilsTests::port_serviceName_ssh()
{
    const auto name = PortScanner::getServiceName(22);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("SSH"), Qt::CaseInsensitive));
}

void NetworkDiagnosticUtilsTests::port_serviceName_ftp()
{
    const auto name = PortScanner::getServiceName(21);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("FTP"), Qt::CaseInsensitive));
}

void NetworkDiagnosticUtilsTests::port_serviceName_dns()
{
    const auto name = PortScanner::getServiceName(53);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("DNS"), Qt::CaseInsensitive));
}

void NetworkDiagnosticUtilsTests::port_serviceName_rdp()
{
    const auto name = PortScanner::getServiceName(3389);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("RDP"), Qt::CaseInsensitive));
}

void NetworkDiagnosticUtilsTests::port_serviceName_unknown()
{
    // Very high ephemeral port unlikely to be in the database
    const auto name = PortScanner::getServiceName(65432);
    QVERIFY(name.isEmpty());
}

// ════════════════════════════════════════════════════════════════════════════
// DNS: wellKnownDnsServers
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::dns_servers_notEmpty()
{
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    QVERIFY(!servers.isEmpty());
    QVERIFY(servers.size() >= 5);
}

void NetworkDiagnosticUtilsTests::dns_servers_haveNames()
{
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    for (const auto& s : servers) {
        QVERIFY2(!s.first.isEmpty(),
                 qPrintable(QStringLiteral("DNS server has empty name")));
    }
}

void NetworkDiagnosticUtilsTests::dns_servers_includeGoogle()
{
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found = false;
    for (const auto& s : servers) {
        if (s.second == QStringLiteral("8.8.8.8")) {
            found = true;
            QVERIFY(s.first.contains(QStringLiteral("Google"), Qt::CaseInsensitive));
            break;
        }
    }
    QVERIFY2(found, "Google DNS (8.8.8.8) not found in server list");
}

void NetworkDiagnosticUtilsTests::dns_servers_includeCloudflare()
{
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found = false;
    for (const auto& s : servers) {
        if (s.second == QStringLiteral("1.1.1.1")) {
            found = true;
            QVERIFY(s.first.contains(QStringLiteral("Cloudflare"), Qt::CaseInsensitive));
            break;
        }
    }
    QVERIFY2(found, "Cloudflare DNS (1.1.1.1) not found in server list");
}

void NetworkDiagnosticUtilsTests::dns_servers_includeSystemDefault()
{
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found = false;
    for (const auto& s : servers) {
        if (s.second.isEmpty()) {
            found = true;
            QVERIFY(s.first.contains(QStringLiteral("Default"), Qt::CaseInsensitive)
                 || s.first.contains(QStringLiteral("System"), Qt::CaseInsensitive));
            break;
        }
    }
    QVERIFY2(found, "System Default DNS entry not found");
}

// ════════════════════════════════════════════════════════════════════════════
// DNS: supportedRecordTypes
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticUtilsTests::dns_recordTypes_notEmpty()
{
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(!types.isEmpty());
}

void NetworkDiagnosticUtilsTests::dns_recordTypes_includeCommon()
{
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains(QStringLiteral("A")));
    QVERIFY(types.contains(QStringLiteral("AAAA")));
    QVERIFY(types.contains(QStringLiteral("MX")));
    QVERIFY(types.contains(QStringLiteral("CNAME")));
}

void NetworkDiagnosticUtilsTests::dns_recordTypes_includeAll()
{
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains(QStringLiteral("TXT")));
    QVERIFY(types.contains(QStringLiteral("SOA")));
    QVERIFY(types.contains(QStringLiteral("NS")));
    QVERIFY(types.contains(QStringLiteral("SRV")));
    QVERIFY(types.contains(QStringLiteral("PTR")));
}

QTEST_GUILESS_MAIN(NetworkDiagnosticUtilsTests)
#include "test_network_diagnostic_utils.moc"
