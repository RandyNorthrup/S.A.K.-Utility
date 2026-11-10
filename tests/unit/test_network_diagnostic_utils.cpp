// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_utils.cpp
/// @brief Unit tests for Network Diagnostic utility / static methods
///
/// Tests WiFi frequency conversion, vendor lookup, channel utilization,
/// port scanner presets and service names, and DNS server/record type lists.

#include "sak/dns_diagnostic_tool.h"
#include "sak/network_diagnostic_types.h"
#include "sak/port_scanner.h"
#include "sak/wifi_analyzer.h"

#include <QtTest/QtTest>

#include <algorithm>

using namespace sak;

class NetworkDiagnosticUtilsTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- WiFi: frequencyToChannel --
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

    // -- WiFi: frequencyToBand --
    void wifi_freqToBand_2_4GHz();
    void wifi_freqToBand_5GHz();
    void wifi_freqToBand_6GHz();
    void wifi_freqToBand_unknown();
    void wifi_freqToBand_boundaries();

    // -- WiFi: lookupVendor --
    void wifi_lookupVendor_knownOui();
    void wifi_lookupVendor_unknownOui();
    void wifi_lookupVendor_tooShort();
    void wifi_lookupVendor_empty();
    void wifi_lookupVendor_caseInsensitive();

    // -- WiFi: calculateChannelUtilization --
    void wifi_channelUtil_emptyInput();
    void wifi_channelUtil_singleNetwork();
    void wifi_channelUtil_multipleOnSameChannel();
    void wifi_channelUtil_multipleChannels();
    void wifi_channelUtil_sortedByChannel();
    void wifi_channelUtil_averageSignal();
    void wifi_channelUtil_interferenceNonNegative();

    // -- PortScanner: getPresets --
    void port_presets_notEmpty();
    void port_presets_haveNames();
    void port_presets_havePorts();
    void port_presets_commonServices();
    void port_presets_top100HasExpectedPorts();

    // -- PortScanner: getServiceName --
    void port_serviceName_http();
    void port_serviceName_https();
    void port_serviceName_ssh();
    void port_serviceName_ftp();
    void port_serviceName_dns();
    void port_serviceName_rdp();
    void port_serviceName_unknown();

    // -- DNS: wellKnownDnsServers --
    void dns_servers_notEmpty();
    void dns_servers_haveNames();
    void dns_servers_includeGoogle();
    void dns_servers_includeCloudflare();
    void dns_servers_includeSystemDefault();

    // -- DNS: supportedRecordTypes --
    void dns_recordTypes_notEmpty();
    void dns_recordTypes_includeCommon();
    void dns_recordTypes_includeAll();

    // -- Port scan: concurrency honors maxConcurrent (B9-19) --
    void port_scan_concurrentReturnsAllPorts();
};

// ============================================================================
// WiFi: frequencyToChannel
// ============================================================================

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel1() {
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'412'000), 1);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel6() {
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'437'000), 6);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel11() {
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'462'000), 11);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel13() {
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'472'000), 13);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_2_4GHz_channel14() {
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'484'000), 14);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_5GHz_channel36() {
    // 5180 MHz = channel 36
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'180'000), 36);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_5GHz_channel44() {
    // 5220 MHz = channel 44
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'220'000), 44);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_5GHz_channel149() {
    // 5745 MHz = channel 149
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'745'000), 149);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_6GHz_channel1() {
    // 5955 MHz = 6 GHz channel 1
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'955'000), 1);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_6GHz_channel5() {
    // 5975 MHz = 6 GHz channel 5
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'975'000), 5);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_unknown() {
    // Out-of-band frequency
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(900'000), 0);
}

void NetworkDiagnosticUtilsTests::wifi_freqToChannel_zero() {
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(0), 0);
}

// ============================================================================
// WiFi: frequencyToBand
// ============================================================================

void NetworkDiagnosticUtilsTests::wifi_freqToBand_2_4GHz() {
    QCOMPARE(WiFiAnalyzer::frequencyToBand(2'412'000), QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(2'437'000), QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(2'484'000), QStringLiteral("2.4 GHz"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_5GHz() {
    QCOMPARE(WiFiAnalyzer::frequencyToBand(5'180'000), QStringLiteral("5 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(5'745'000), QStringLiteral("5 GHz"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_6GHz() {
    QCOMPARE(WiFiAnalyzer::frequencyToBand(5'955'000), QStringLiteral("6 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(7'000'000), QStringLiteral("6 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(7'115'000), QStringLiteral("6 GHz"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_unknown() {
    QCOMPARE(WiFiAnalyzer::frequencyToBand(900'000), QStringLiteral("Unknown"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(0), QStringLiteral("Unknown"));
}

void NetworkDiagnosticUtilsTests::wifi_freqToBand_boundaries() {
    // Start boundaries
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq2GHzStart), QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq5GHzStart), QStringLiteral("5 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq6GHzStart), QStringLiteral("6 GHz"));

    // End boundaries
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq2GHzEnd), QStringLiteral("2.4 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq5GHzEnd), QStringLiteral("5 GHz"));
    QCOMPARE(WiFiAnalyzer::frequencyToBand(netdiag::kFreq6GHzEnd), QStringLiteral("6 GHz"));
}

// ============================================================================
// WiFi: lookupVendor
// ============================================================================

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_knownOui() {
    // No oui_database.txt ships, so the built-in fallback set IS the database and its entries are
    // deterministic. "AA:BB:CC" is in no set at all, so the old input proved nothing about a
    // lookup ever succeeding; a seeded OUI pins the vendor the caller is shown.
    const auto result = WiFiAnalyzer::lookupVendor(QStringLiteral("00:50:56:11:22:33"));
    QCOMPARE(result, QStringLiteral("VMware"));
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_unknownOui() {
    // An unseeded OUI resolves to nothing -- and specifically NOT to a "closest match" or a
    // placeholder vendor string the UI would then display as fact.
    const auto result = WiFiAnalyzer::lookupVendor(QStringLiteral("00:00:00:00:00:00"));
    QCOMPARE(result, QString());
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_tooShort() {
    const auto result = WiFiAnalyzer::lookupVendor(QStringLiteral("AA:BB"));
    QVERIFY(result.isEmpty());
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_empty() {
    const auto result = WiFiAnalyzer::lookupVendor(QString());
    QVERIFY(result.isEmpty());
}

void NetworkDiagnosticUtilsTests::wifi_lookupVendor_caseInsensitive() {
    // Use a SEEDED OUI (AC:DE:48 -> Apple). "AA:BB:CC" is not in the fallback DB, so both lookups
    // returned empty and the case-fold under test (lookupVendor's .toUpper()) was never exercised
    // -- dropping it would still have passed. The lowercase form only resolves because of that
    // fold.
    const auto upper = WiFiAnalyzer::lookupVendor(QStringLiteral("AC:DE:48:00:11:22"));
    const auto lower = WiFiAnalyzer::lookupVendor(QStringLiteral("ac:de:48:00:11:22"));
    QCOMPARE(upper, QStringLiteral("Apple"));
    QCOMPARE(lower, QStringLiteral("Apple"));
    QCOMPARE(upper, lower);
}

// ============================================================================
// WiFi: calculateChannelUtilization
// ============================================================================

void NetworkDiagnosticUtilsTests::wifi_channelUtil_emptyInput() {
    QVector<WiFiNetworkInfo> empty;
    const auto result = WiFiAnalyzer::calculateChannelUtilization(empty);
    QVERIFY(result.isEmpty());
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_singleNetwork() {
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
    // The remaining fields of the entry are equally deterministic: the band is carried through
    // from the network, and the score is 1 * (1 - (-50 / -80)) == 0.375.
    QCOMPARE(result[0].band, QStringLiteral("2.4 GHz"));
    QCOMPARE(result[0].averageSignalDbm, -50.0);
    QCOMPARE(result[0].interferenceScore, 0.375);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_multipleOnSameChannel() {
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
    // Both SSIDs are collected, in encounter order -- a size-only check cannot catch one SSID
    // being recorded twice while the other is dropped.
    QCOMPARE(result[0].ssids, (QVector<QString>{QStringLiteral("Net1"), QStringLiteral("Net2")}));
    QCOMPARE(result[0].band, QStringLiteral("2.4 GHz"));
    QCOMPARE(result[0].averageSignalDbm, -60.0);
    // 2 networks * (1 - (-60 / -80)) == 0.5
    QCOMPARE(result[0].interferenceScore, 0.5);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_multipleChannels() {
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
    // Three distinct channels means three one-network entries, each carrying its OWN network --
    // a bucketing bug that merged or cross-assigned them still yields size 3.
    QCOMPARE(result[0].channelNumber, 1);
    QCOMPARE(result[1].channelNumber, 6);
    QCOMPARE(result[2].channelNumber, 11);
    QCOMPARE(result[0].ssids, (QVector<QString>{QStringLiteral("Net1")}));
    QCOMPARE(result[1].ssids, (QVector<QString>{QStringLiteral("Net2")}));
    QCOMPARE(result[2].ssids, (QVector<QString>{QStringLiteral("Net3")}));
    QCOMPARE(result[0].networkCount, 1);
    QCOMPARE(result[1].networkCount, 1);
    QCOMPARE(result[2].networkCount, 1);
    QCOMPARE(result[0].averageSignalDbm, -50.0);
    QCOMPARE(result[1].averageSignalDbm, -65.0);
    QCOMPARE(result[2].averageSignalDbm, -75.0);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_sortedByChannel() {
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
    // The per-channel payload must travel WITH its channel through the sort: each entry keeps
    // the signal of the network that was added at that channel (ch1 -60, ch6 -55, ch11 -50 --
    // deliberately the reverse of channel order, so a payload left behind by the sort shows up).
    QCOMPARE(result[0].averageSignalDbm, -60.0);
    QCOMPARE(result[1].averageSignalDbm, -55.0);
    QCOMPARE(result[2].averageSignalDbm, -50.0);
}

void NetworkDiagnosticUtilsTests::wifi_channelUtil_averageSignal() {
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

void NetworkDiagnosticUtilsTests::wifi_channelUtil_interferenceNonNegative() {
    QVector<WiFiNetworkInfo> networks;

    // Very weak signals (weaker than kSignalWeak = -80)
    WiFiNetworkInfo net;
    net.channelNumber = 1;
    net.rssiDbm = -90;
    networks.append(net);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    QCOMPARE(result.size(), 1);
    // -90 dBm is weaker than kSignalWeak (-80), so the raw score is negative and the clamp is
    // what produces the result: pin the exact clamped value, since ">= 0.0" also holds for
    // every un-clamped positive score and so cannot fail if the clamp were removed.
    QCOMPARE(result[0].interferenceScore, 0.0);
}

// ============================================================================
// PortScanner: getPresets
// ============================================================================

void NetworkDiagnosticUtilsTests::port_presets_notEmpty() {
    const auto presets = PortScanner::getPresets();
    // Fixed catalog: Common Services, Web Servers, Database, File Sharing, Email, Remote Access,
    // Top 100 = 7. `>= 5` would miss a dropped preset.
    QCOMPARE(presets.size(), 7);
}

void NetworkDiagnosticUtilsTests::port_presets_haveNames() {
    // The catalog is a fixed literal, so the names the technician picks from are deterministic
    // in both spelling AND order -- a non-empty check would not notice a renamed or reordered
    // preset (the combo-box selection is by row).
    const auto presets = PortScanner::getPresets();
    QCOMPARE(presets.size(), 7);
    QCOMPARE(presets[0].name, QStringLiteral("Common Services"));
    QCOMPARE(presets[1].name, QStringLiteral("Web Servers"));
    QCOMPARE(presets[2].name, QStringLiteral("Database"));
    QCOMPARE(presets[3].name, QStringLiteral("File Sharing"));
    QCOMPARE(presets[4].name, QStringLiteral("Email"));
    QCOMPARE(presets[5].name, QStringLiteral("Remote Access"));
    QCOMPARE(presets[6].name, QStringLiteral("Top 100"));
}

void NetworkDiagnosticUtilsTests::port_presets_havePorts() {
    // Each preset's port count is fixed; a dropped port still leaves the list non-empty.
    const auto presets = PortScanner::getPresets();
    QCOMPARE(presets.size(), 7);
    QCOMPARE(presets[0].ports.size(), 21);  // Common Services
    QCOMPARE(presets[1].ports.size(), 8);   // Web Servers
    QCOMPARE(presets[2].ports.size(), 7);   // Database
    QCOMPARE(presets[3].ports.size(), 11);  // File Sharing
    QCOMPARE(presets[4].ports.size(), 7);   // Email
    QCOMPARE(presets[5].ports.size(), 7);   // Remote Access
    QCOMPARE(presets[6].ports.size(), 98);  // Top 100
}

void NetworkDiagnosticUtilsTests::port_presets_commonServices() {
    // The preset IS the scan the technician runs, so pin the whole ordered port list rather
    // than four spot-checks: a silently added port scans a host the operator did not intend,
    // and a silently dropped one leaves a service unscanned. Both pass a contains() check.
    const auto presets = PortScanner::getPresets();
    QCOMPARE(presets.size(), 7);
    QCOMPARE(presets[0].name, QStringLiteral("Common Services"));
    QCOMPARE(presets[0].ports,
             (QVector<uint16_t>{20,  21,  22,  23,  25,  53,   80,   110,  115,  135, 139,
                                143, 443, 445, 993, 995, 1723, 3306, 3389, 5900, 8080}));
}

void NetworkDiagnosticUtilsTests::port_presets_top100HasExpectedPorts() {
    const auto presets = PortScanner::getPresets();

    // Same reasoning as Common Services: the "Top 100" list (98 entries despite the name) is a
    // fixed literal, so pin it whole and in order.
    QCOMPARE(presets.size(), 7);
    QCOMPARE(presets[6].name, QStringLiteral("Top 100"));
    QCOMPARE(presets[6].ports,
             (QVector<uint16_t>{7,    9,    13,   21,     22,     23,     25,     26,    37,   53,
                                79,   80,   81,   88,     106,    110,    111,    113,   119,  135,
                                139,  143,  144,  179,    199,    389,    427,    443,   444,  445,
                                465,  513,  514,  515,    543,    544,    548,    554,   587,  631,
                                646,  873,  990,  993,    995,    1025,   1026,   1027,  1028, 1029,
                                1110, 1433, 1720, 1723,   1755,   1900,   2000,   2001,  2049, 2121,
                                2717, 3000, 3128, 3306,   3389,   3986,   4899,   5000,  5009, 5051,
                                5060, 5101, 5190, 5357,   5432,   5631,   5666,   5800,  5900, 5901,
                                6000, 6001, 6646, 7070,   8000,   8008,   8009,   8080,  8081, 8443,
                                8888, 9100, 9999, 10'000, 32'768, 49'152, 49'153, 49'154}));
}

// ============================================================================
// PortScanner: getServiceName
// ============================================================================

void NetworkDiagnosticUtilsTests::port_serviceName_http() {
    const auto name = PortScanner::getServiceName(80);
    // Exact: contains("HTTP") also matches HTTPS / HTTP Proxy / HTTP Alt, so an off-by-one onto an
    // HTTP* neighbor would stay green.
    QCOMPARE(name, QStringLiteral("HTTP"));
}

void NetworkDiagnosticUtilsTests::port_serviceName_https() {
    const auto name = PortScanner::getServiceName(443);
    QCOMPARE(name, QStringLiteral("HTTPS"));
}

void NetworkDiagnosticUtilsTests::port_serviceName_ssh() {
    const auto name = PortScanner::getServiceName(22);
    QCOMPARE(name, QStringLiteral("SSH"));
}

void NetworkDiagnosticUtilsTests::port_serviceName_ftp() {
    const auto name = PortScanner::getServiceName(21);
    // Exact: contains("FTP") also matches port 20's "FTP Data", so a 20/21 swap would stay green.
    QCOMPARE(name, QStringLiteral("FTP Control"));
}

void NetworkDiagnosticUtilsTests::port_serviceName_dns() {
    const auto name = PortScanner::getServiceName(53);
    // Exact: contains("DNS") also matches port 5353's "mDNS".
    QCOMPARE(name, QStringLiteral("DNS"));
}

void NetworkDiagnosticUtilsTests::port_serviceName_rdp() {
    const auto name = PortScanner::getServiceName(3389);
    QCOMPARE(name, QStringLiteral("RDP"));
}

void NetworkDiagnosticUtilsTests::port_serviceName_unknown() {
    // Very high ephemeral port unlikely to be in the database
    const auto name = PortScanner::getServiceName(65'432);
    QVERIFY(name.isEmpty());
}

// ============================================================================
// DNS: wellKnownDnsServers
// ============================================================================

void NetworkDiagnosticUtilsTests::dns_servers_notEmpty() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    // Fixed list: System Default + Google(x2) + Cloudflare(x2) + Quad9(x2) + OpenDNS(x2) = 9.
    QCOMPARE(servers.size(), 9);
}

void NetworkDiagnosticUtilsTests::dns_servers_haveNames() {
    // The catalog is a fixed literal and the name/address PAIRING is the security-relevant part:
    // a name attached to the wrong resolver address would send the technician's queries somewhere
    // other than the provider they picked, and a non-empty-name check cannot see that.
    const QVector<QPair<QString, QString>> kExpected = {
        {QStringLiteral("System Default"), QString()},
        {QStringLiteral("Google DNS"), QStringLiteral("8.8.8.8")},
        {QStringLiteral("Google DNS (Secondary)"), QStringLiteral("8.8.4.4")},
        {QStringLiteral("Cloudflare"), QStringLiteral("1.1.1.1")},
        {QStringLiteral("Cloudflare (Secondary)"), QStringLiteral("1.0.0.1")},
        {QStringLiteral("Quad9"), QStringLiteral("9.9.9.9")},
        {QStringLiteral("Quad9 (Secondary)"), QStringLiteral("149.112.112.112")},
        {QStringLiteral("OpenDNS"), QStringLiteral("208.67.222.222")},
        {QStringLiteral("OpenDNS (Secondary)"), QStringLiteral("208.67.220.220")},
    };
    QCOMPARE(DnsDiagnosticTool::wellKnownDnsServers(), kExpected);
}

void NetworkDiagnosticUtilsTests::dns_servers_includeGoogle() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found = false;
    for (const auto& s : servers) {
        if (s.second == QStringLiteral("8.8.8.8")) {
            found = true;
            QCOMPARE(s.first, QStringLiteral("Google DNS"));
            break;
        }
    }
    QVERIFY2(found, "Google DNS (8.8.8.8) not found in server list");
}

void NetworkDiagnosticUtilsTests::dns_servers_includeCloudflare() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found = false;
    for (const auto& s : servers) {
        if (s.second == QStringLiteral("1.1.1.1")) {
            found = true;
            // The exact label, not merely one containing "Cloudflare": the SECONDARY entry also
            // contains it, so a mispaired address would still pass a contains() check.
            QCOMPARE(s.first, QStringLiteral("Cloudflare"));
            break;
        }
    }
    QVERIFY2(found, "Cloudflare DNS (1.1.1.1) not found in server list");
}

void NetworkDiagnosticUtilsTests::dns_servers_includeSystemDefault() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found = false;
    for (const auto& s : servers) {
        if (s.second.isEmpty()) {
            found = true;
            // The empty-address entry is exactly "System Default".
            QCOMPARE(s.first, QStringLiteral("System Default"));
            break;
        }
    }
    QVERIFY2(found, "System Default DNS entry not found");
}

// ============================================================================
// DNS: supportedRecordTypes
// ============================================================================

void NetworkDiagnosticUtilsTests::dns_recordTypes_notEmpty() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    // Fixed list: A, AAAA, MX, CNAME, TXT, SOA, NS, SRV, PTR. The two tests below assert every
    // one of those is PRESENT; pinning the count here is what makes them a closed set, so an
    // extra unhandled record type offered in the UI cannot slip in unnoticed.
    QCOMPARE(types.size(), 9);
}

void NetworkDiagnosticUtilsTests::dns_recordTypes_includeCommon() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains(QStringLiteral("A")));
    QVERIFY(types.contains(QStringLiteral("AAAA")));
    QVERIFY(types.contains(QStringLiteral("MX")));
    QVERIFY(types.contains(QStringLiteral("CNAME")));
}

void NetworkDiagnosticUtilsTests::dns_recordTypes_includeAll() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains(QStringLiteral("TXT")));
    QVERIFY(types.contains(QStringLiteral("SOA")));
    QVERIFY(types.contains(QStringLiteral("NS")));
    QVERIFY(types.contains(QStringLiteral("SRV")));
    QVERIFY(types.contains(QStringLiteral("PTR")));
}

// ============================================================================
// Port scan: batched concurrency honors maxConcurrent (B9-19)
// ============================================================================

void NetworkDiagnosticUtilsTests::port_scan_concurrentReturnsAllPorts() {
    // Scan several localhost ports with maxConcurrent > 1: the batched path must
    // return a result for EVERY requested port exactly once (previously the config
    // field was ignored and the scan ran strictly serially).
    PortScanner scanner;
    QVector<PortScanResult> completed;
    int completeCount = 0;
    QObject::connect(
        &scanner, &PortScanner::scanComplete, &scanner, [&](const QVector<PortScanResult>& r) {
            completed = r;
            ++completeCount;
        });

    PortScanner::ScanConfig config;
    config.target = QStringLiteral("127.0.0.1");
    config.ports = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    config.timeoutMs = 500;
    config.grabBanners = false;
    config.maxConcurrent = 4;

    scanner.scan(config);  // blocking; emits scanComplete synchronously when done

    QCOMPARE(completeCount, 1);
    QCOMPARE(completed.size(), config.ports.size());

    // Batches are joined in order, so results come back in the REQUESTED port order -- pin that
    // index-for-index (a set of seen ports proves only that each port appears somewhere, not that
    // a result carries the port it was actually probed for), along with the target each result
    // is attributed to.
    for (int i = 0; i < completed.size(); ++i) {
        QCOMPARE(completed[i].port, config.ports[i]);
        QCOMPARE(completed[i].target, QStringLiteral("127.0.0.1"));
    }
}

QTEST_GUILESS_MAIN(NetworkDiagnosticUtilsTests)
#include "test_network_diagnostic_utils.moc"
