// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_types.cpp
/// @brief Unit tests for Network Diagnostic shared data types
///
/// Validates default construction, value/move semantics, enum values,
/// and compile-time invariants for all network diagnostic structs.

#include "sak/network_diagnostic_types.h"

#include <QtTest/QtTest>

#include <algorithm>
#include <type_traits>

using namespace sak;

class NetworkDiagnosticTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── netdiag constants ──
    void constants_pingDefaults();
    void constants_tracerouteDefaults();
    void constants_portScanDefaults();
    void constants_bandwidthDefaults();
    void constants_wifiDefaults();
    void constants_frequencyBoundaries();
    void constants_signalThresholds();

    // ── NetworkAdapterInfo ──
    void adapterInfo_defaultConstruction();
    void adapterInfo_valueSemantics();
    void adapterInfo_moveSemantics();

    // ── PingReply ──
    void pingReply_defaultConstruction();
    void pingReply_valueSemantics();

    // ── PingResult ──
    void pingResult_defaultConstruction();
    void pingResult_valueSemantics();

    // ── TracerouteHop ──
    void tracerouteHop_defaultConstruction();
    void tracerouteHop_valueSemantics();

    // ── TracerouteResult ──
    void tracerouteResult_defaultConstruction();

    // ── MtrHopStats ──
    void mtrHopStats_defaultConstruction();
    void mtrHopStats_valueSemantics();

    // ── MtrResult ──
    void mtrResult_defaultConstruction();

    // ── DnsQueryResult ──
    void dnsQueryResult_defaultConstruction();
    void dnsQueryResult_valueSemantics();

    // ── DnsServerComparison ──
    void dnsComparison_defaultConstruction();

    // ── PortScanResult ──
    void portScanResult_defaultConstruction();
    void portScanResult_stateEnum();
    void portScanResult_valueSemantics();

    // ── PortPreset ──
    void portPreset_defaultConstruction();
    void portPreset_valueSemantics();

    // ── BandwidthTestResult ──
    void bandwidthResult_defaultConstruction();
    void bandwidthResult_testModeEnum();
    void bandwidthResult_intervalData();

    // ── WiFiNetworkInfo ──
    void wifiNetworkInfo_defaultConstruction();
    void wifiNetworkInfo_valueSemantics();

    // ── WiFiChannelUtilization ──
    void wifiChannelUtil_defaultConstruction();

    // ── ConnectionInfo ──
    void connectionInfo_defaultConstruction();
    void connectionInfo_protocolEnum();

    // ── FirewallRule ──
    void firewallRule_defaultConstruction();
    void firewallRule_directionEnum();
    void firewallRule_actionEnum();
    void firewallRule_protocolEnum();
    void firewallRule_profileEnum();

    // ── FirewallConflict ──
    void firewallConflict_defaultConstruction();
    void firewallConflict_severityEnum();

    // ── FirewallGap ──
    void firewallGap_defaultConstruction();
    void firewallGap_severityEnum();

    // ── NetworkShareInfo ──
    void shareInfo_defaultConstruction();
    void shareInfo_shareTypeEnum();

    // ── Compile-Time Invariants ──
    void staticAsserts_defaultConstructible();
    void staticAsserts_copyConstructible();
    void staticAsserts_movable();
};

// ════════════════════════════════════════════════════════════════════════════
// netdiag Constants
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::constants_pingDefaults() {
    QCOMPARE(netdiag::kDefaultPingCount, 10);
    QCOMPARE(netdiag::kDefaultPingIntervalMs, 1000);
    QCOMPARE(netdiag::kDefaultPingTimeoutMs, 4000);
    QCOMPARE(netdiag::kDefaultPingPacketSize, 32);
    QCOMPARE(netdiag::kDefaultPingTtl, 128);
}

void NetworkDiagnosticTypesTests::constants_tracerouteDefaults() {
    QCOMPARE(netdiag::kDefaultTracerouteMaxHops, 30);
    QCOMPARE(netdiag::kDefaultTracerouteTimeout, 5000);
    QCOMPARE(netdiag::kDefaultTracerouteProbes, 3);
    QCOMPARE(netdiag::kDefaultMtrCycles, 100);
}

void NetworkDiagnosticTypesTests::constants_portScanDefaults() {
    QCOMPARE(netdiag::kDefaultPortScanTimeoutMs, 3000);
    QCOMPARE(netdiag::kDefaultMaxConcurrent, 50);
    QCOMPARE(netdiag::kBannerGrabTimeoutMs, 2000);
    QCOMPARE(netdiag::kBannerMaxBytes, 512);
}

void NetworkDiagnosticTypesTests::constants_bandwidthDefaults() {
    QCOMPARE(netdiag::kDefaultBandwidthDuration, 10);
    QCOMPARE(netdiag::kDefaultIperfPort, static_cast<uint16_t>(5201));
}

void NetworkDiagnosticTypesTests::constants_wifiDefaults() {
    QCOMPARE(netdiag::kDefaultWifiScanIntervalMs, 5000);
    QCOMPARE(netdiag::kDefaultConnRefreshMs, 2000);
}

void NetworkDiagnosticTypesTests::constants_frequencyBoundaries() {
    // 2.4 GHz band
    QCOMPARE(netdiag::kFreq2GHzStart, 2'412'000u);
    QCOMPARE(netdiag::kFreq2GHzEnd, 2'484'000u);

    // 5 GHz band
    QCOMPARE(netdiag::kFreq5GHzStart, 5'170'000u);
    QCOMPARE(netdiag::kFreq5GHzEnd, 5'835'000u);

    // 6 GHz band
    QCOMPARE(netdiag::kFreq6GHzStart, 5'955'000u);
    QCOMPARE(netdiag::kFreq6GHzEnd, 7'115'000u);

    // Bands must be non-overlapping
    QVERIFY(netdiag::kFreq2GHzEnd < netdiag::kFreq5GHzStart);
    QVERIFY(netdiag::kFreq5GHzEnd < netdiag::kFreq6GHzStart);
}

void NetworkDiagnosticTypesTests::constants_signalThresholds() {
    QCOMPARE(netdiag::kSignalExcellent, -50);
    QCOMPARE(netdiag::kSignalGood, -60);
    QCOMPARE(netdiag::kSignalFair, -70);
    QCOMPARE(netdiag::kSignalWeak, -80);

    // Must be in descending order (stronger → weaker)
    QVERIFY(netdiag::kSignalExcellent > netdiag::kSignalGood);
    QVERIFY(netdiag::kSignalGood > netdiag::kSignalFair);
    QVERIFY(netdiag::kSignalFair > netdiag::kSignalWeak);
}

// ════════════════════════════════════════════════════════════════════════════
// NetworkAdapterInfo
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::adapterInfo_defaultConstruction() {
    NetworkAdapterInfo info;

    QVERIFY(info.name.isEmpty());
    QVERIFY(info.description.isEmpty());
    QVERIFY(info.macAddress.isEmpty());
    QVERIFY(info.ipv4Addresses.isEmpty());
    QVERIFY(info.ipv6Addresses.isEmpty());
    QVERIFY(info.ipv4Gateway.isEmpty());
    QVERIFY(info.ipv6Gateway.isEmpty());
    QVERIFY(info.ipv4DnsServers.isEmpty());
    QVERIFY(info.ipv6DnsServers.isEmpty());
    QVERIFY(info.adapterType.isEmpty());
    QVERIFY(!info.dhcpEnabled);
    QVERIFY(!info.isConnected);
    QCOMPARE(info.interfaceIndex, 0u);
    QCOMPARE(info.linkSpeedBps, static_cast<uint64_t>(0));
}

void NetworkDiagnosticTypesTests::adapterInfo_valueSemantics() {
    NetworkAdapterInfo original;
    original.name = QStringLiteral("eth0");
    original.description = QStringLiteral("Ethernet Adapter");
    original.macAddress = QStringLiteral("AA:BB:CC:DD:EE:FF");
    original.ipv4Addresses = {QStringLiteral("192.168.1.100")};
    original.ipv6Addresses = {QStringLiteral("fe80::1")};
    original.ipv4Gateway = QStringLiteral("192.168.1.1");
    original.ipv6Gateway = QStringLiteral("fe80::1");
    original.ipv4DnsServers = {QStringLiteral("8.8.8.8")};
    original.ipv6DnsServers = {QStringLiteral("2001:4860:4860::8888")};
    original.adapterType = QStringLiteral("Ethernet");
    original.dhcpEnabled = true;
    original.isConnected = true;
    original.interfaceIndex = 1;
    original.linkSpeedBps = 1'000'000'000;

    // Copy
    NetworkAdapterInfo copy = original;
    QCOMPARE(copy.name, original.name);
    QCOMPARE(copy.macAddress, original.macAddress);
    QCOMPARE(copy.ipv4Addresses, original.ipv4Addresses);
    QCOMPARE(copy.dhcpEnabled, original.dhcpEnabled);
    QCOMPARE(copy.interfaceIndex, original.interfaceIndex);
    QCOMPARE(copy.linkSpeedBps, original.linkSpeedBps);

    // Copy independence
    copy.name = QStringLiteral("eth1");
    QCOMPARE(original.name, QStringLiteral("eth0"));
}

void NetworkDiagnosticTypesTests::adapterInfo_moveSemantics() {
    NetworkAdapterInfo original;
    original.name = QStringLiteral("wlan0");
    original.macAddress = QStringLiteral("11:22:33:44:55:66");

    NetworkAdapterInfo moved = std::move(original);
    QCOMPARE(moved.name, QStringLiteral("wlan0"));
    QCOMPARE(moved.macAddress, QStringLiteral("11:22:33:44:55:66"));
}

// ════════════════════════════════════════════════════════════════════════════
// PingReply / PingResult
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::pingReply_defaultConstruction() {
    PingReply reply;

    QCOMPARE(reply.rttMs, 0.0);
    QCOMPARE(reply.ttl, 0);
    QVERIFY(reply.replyFrom.isEmpty());
    QVERIFY(!reply.success);
    QVERIFY(reply.errorMessage.isEmpty());
}

void NetworkDiagnosticTypesTests::pingReply_valueSemantics() {
    PingReply original;
    original.rttMs = 12.5;
    original.ttl = 64;
    original.replyFrom = QStringLiteral("8.8.8.8");
    original.success = true;

    PingReply copy = original;
    QCOMPARE(copy.rttMs, 12.5);
    QCOMPARE(copy.ttl, 64);
    QCOMPARE(copy.replyFrom, QStringLiteral("8.8.8.8"));
}

void NetworkDiagnosticTypesTests::pingResult_defaultConstruction() {
    PingResult result;

    QVERIFY(result.target.isEmpty());
    QVERIFY(result.resolvedIP.isEmpty());
    QCOMPARE(result.sent, 0);
    QCOMPARE(result.received, 0);
    QCOMPARE(result.lossPercent, 0.0);
    QCOMPARE(result.avgRtt, 0.0);
    QCOMPARE(result.minRtt, 0.0);
    QCOMPARE(result.maxRtt, 0.0);
    QCOMPARE(result.jitter, 0.0);
    QVERIFY(result.replies.isEmpty());
}

void NetworkDiagnosticTypesTests::pingResult_valueSemantics() {
    PingResult original;
    original.target = QStringLiteral("google.com");
    original.resolvedIP = QStringLiteral("8.8.8.8");
    original.sent = 10;
    original.received = 9;
    original.lossPercent = 10.0;
    original.avgRtt = 15.5;
    original.minRtt = 10.0;
    original.maxRtt = 25.0;
    original.jitter = 3.2;

    PingReply reply;
    reply.rttMs = 15.0;
    reply.success = true;
    original.replies.append(reply);

    PingResult copy = original;
    QCOMPARE(copy.target, original.target);
    QCOMPARE(copy.sent, 10);
    QCOMPARE(copy.replies.size(), 1);
    QCOMPARE(copy.replies[0].rttMs, 15.0);
}

// ════════════════════════════════════════════════════════════════════════════
// TracerouteHop / TracerouteResult
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::tracerouteHop_defaultConstruction() {
    TracerouteHop hop;

    QCOMPARE(hop.hopNumber, 0);
    QVERIFY(hop.ipAddress.isEmpty());
    QVERIFY(hop.hostname.isEmpty());
    QCOMPARE(hop.rtt1Ms, 0.0);
    QCOMPARE(hop.rtt2Ms, 0.0);
    QCOMPARE(hop.rtt3Ms, 0.0);
    QCOMPARE(hop.avgRttMs, 0.0);
    QVERIFY(!hop.timedOut);
}

void NetworkDiagnosticTypesTests::tracerouteHop_valueSemantics() {
    TracerouteHop original;
    original.hopNumber = 3;
    original.ipAddress = QStringLiteral("10.0.0.1");
    original.hostname = QStringLiteral("router.local");
    original.avgRttMs = 5.3;

    TracerouteHop copy = original;
    QCOMPARE(copy.hopNumber, 3);
    QCOMPARE(copy.ipAddress, QStringLiteral("10.0.0.1"));
}

void NetworkDiagnosticTypesTests::tracerouteResult_defaultConstruction() {
    TracerouteResult result;

    QVERIFY(result.target.isEmpty());
    QVERIFY(result.resolvedIP.isEmpty());
    QVERIFY(result.hops.isEmpty());
    QVERIFY(!result.reachedTarget);
    QCOMPARE(result.totalHops, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// MtrHopStats / MtrResult
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::mtrHopStats_defaultConstruction() {
    MtrHopStats stats;

    QCOMPARE(stats.hopNumber, 0);
    QVERIFY(stats.ipAddress.isEmpty());
    QVERIFY(stats.hostname.isEmpty());
    QCOMPARE(stats.lossPercent, 0.0);
    QCOMPARE(stats.sent, 0);
    QCOMPARE(stats.received, 0);
    QCOMPARE(stats.avgRttMs, 0.0);
    QCOMPARE(stats.bestRttMs, 0.0);
    QCOMPARE(stats.worstRttMs, 0.0);
    QCOMPARE(stats.jitterMs, 0.0);
}

void NetworkDiagnosticTypesTests::mtrHopStats_valueSemantics() {
    MtrHopStats original;
    original.hopNumber = 5;
    original.ipAddress = QStringLiteral("172.16.0.1");
    original.avgRttMs = 12.5;
    original.bestRttMs = 10.0;
    original.worstRttMs = 18.0;
    original.jitterMs = 2.1;
    original.lossPercent = 5.0;
    original.sent = 100;
    original.received = 95;

    MtrHopStats copy = original;
    QCOMPARE(copy.hopNumber, 5);
    QCOMPARE(copy.avgRttMs, 12.5);
    QCOMPARE(copy.lossPercent, 5.0);
}

void NetworkDiagnosticTypesTests::mtrResult_defaultConstruction() {
    MtrResult result;

    QVERIFY(result.target.isEmpty());
    QVERIFY(result.hops.isEmpty());
    QCOMPARE(result.totalCycles, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// DnsQueryResult / DnsServerComparison
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::dnsQueryResult_defaultConstruction() {
    DnsQueryResult result;

    QVERIFY(result.queryName.isEmpty());
    QVERIFY(result.recordType.isEmpty());
    QVERIFY(result.dnsServer.isEmpty());
    QVERIFY(result.answers.isEmpty());
    QCOMPARE(result.responseTimeMs, 0.0);
    QVERIFY(!result.success);
}

void NetworkDiagnosticTypesTests::dnsQueryResult_valueSemantics() {
    DnsQueryResult original;
    original.queryName = QStringLiteral("example.com");
    original.recordType = QStringLiteral("A");
    original.dnsServer = QStringLiteral("8.8.8.8");
    original.answers = {QStringLiteral("93.184.216.34")};
    original.responseTimeMs = 25.0;
    original.success = true;

    DnsQueryResult copy = original;
    QCOMPARE(copy.queryName, QStringLiteral("example.com"));
    QCOMPARE(copy.answers.size(), 1);
    QCOMPARE(copy.responseTimeMs, 25.0);
}

void NetworkDiagnosticTypesTests::dnsComparison_defaultConstruction() {
    DnsServerComparison comparison;

    QVERIFY(comparison.queryName.isEmpty());
    QVERIFY(comparison.recordType.isEmpty());
    QVERIFY(comparison.results.isEmpty());
    QVERIFY(!comparison.allAgree);
}

// ════════════════════════════════════════════════════════════════════════════
// PortScanResult / PortPreset
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::portScanResult_defaultConstruction() {
    PortScanResult result;

    QCOMPARE(result.port, static_cast<uint16_t>(0));
    QCOMPARE(result.state, PortScanResult::State::Error);
    QVERIFY(result.serviceName.isEmpty());
    QVERIFY(result.banner.isEmpty());
    QCOMPARE(result.responseTimeMs, 0.0);
}

void NetworkDiagnosticTypesTests::portScanResult_stateEnum() {
    // Verify all states are distinct
    QVERIFY(PortScanResult::State::Open != PortScanResult::State::Closed);
    QVERIFY(PortScanResult::State::Closed != PortScanResult::State::Filtered);

    // Verify assignment works
    PortScanResult result;
    result.state = PortScanResult::State::Open;
    QCOMPARE(result.state, PortScanResult::State::Open);
    result.state = PortScanResult::State::Filtered;
    QCOMPARE(result.state, PortScanResult::State::Filtered);
}

void NetworkDiagnosticTypesTests::portScanResult_valueSemantics() {
    PortScanResult original;
    original.port = 443;
    original.state = PortScanResult::State::Open;
    original.serviceName = QStringLiteral("HTTPS");
    original.banner = QStringLiteral("nginx/1.25");
    original.responseTimeMs = 5.2;

    PortScanResult copy = original;
    QCOMPARE(copy.port, static_cast<uint16_t>(443));
    QCOMPARE(copy.state, PortScanResult::State::Open);
    QCOMPARE(copy.serviceName, QStringLiteral("HTTPS"));
    QCOMPARE(copy.banner, QStringLiteral("nginx/1.25"));
}

void NetworkDiagnosticTypesTests::portPreset_defaultConstruction() {
    PortPreset preset;
    QVERIFY(preset.name.isEmpty());
    QVERIFY(preset.ports.isEmpty());
}

void NetworkDiagnosticTypesTests::portPreset_valueSemantics() {
    PortPreset original;
    original.name = QStringLiteral("Web");
    original.ports = {80, 443, 8080};

    PortPreset copy = original;
    QCOMPARE(copy.name, QStringLiteral("Web"));
    QCOMPARE(copy.ports.size(), 3);
    QCOMPARE(copy.ports[0], static_cast<uint16_t>(80));
}

// ════════════════════════════════════════════════════════════════════════════
// BandwidthTestResult
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::bandwidthResult_defaultConstruction() {
    BandwidthTestResult result;

    QCOMPARE(result.downloadMbps, 0.0);
    QCOMPARE(result.uploadMbps, 0.0);
    QCOMPARE(result.jitterMs, 0.0);
    QCOMPARE(result.packetLossPercent, 0.0);
    QCOMPARE(result.retransmissions, 0.0);
    QVERIFY(result.intervals.isEmpty());
    QVERIFY(result.target.isEmpty());
}

void NetworkDiagnosticTypesTests::bandwidthResult_testModeEnum() {
    QVERIFY(BandwidthTestResult::TestMode::LanIperf3 != BandwidthTestResult::TestMode::WanHttp);
}

void NetworkDiagnosticTypesTests::bandwidthResult_intervalData() {
    BandwidthTestResult::IntervalData interval;
    QCOMPARE(interval.startSec, 0.0);
    QCOMPARE(interval.endSec, 0.0);
    QCOMPARE(interval.bitsPerSecond, 0.0);
    QCOMPARE(interval.retransmits, 0);

    interval.startSec = 0.0;
    interval.endSec = 1.0;
    interval.bitsPerSecond = 1000000000.0;  // 1 Gbps
    interval.retransmits = 5;

    BandwidthTestResult result;
    result.intervals.append(interval);
    QCOMPARE(result.intervals.size(), 1);
    QCOMPARE(result.intervals[0].bitsPerSecond, 1000000000.0);
}

// ════════════════════════════════════════════════════════════════════════════
// WiFiNetworkInfo / WiFiChannelUtilization
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::wifiNetworkInfo_defaultConstruction() {
    WiFiNetworkInfo info;

    QVERIFY(info.ssid.isEmpty());
    QVERIFY(info.bssid.isEmpty());
    QCOMPARE(info.rssiDbm, 0);
    QCOMPARE(info.signalQuality, 0);
    QCOMPARE(info.channelFrequencyKHz, 0u);
    QCOMPARE(info.channelNumber, 0);
    QVERIFY(info.band.isEmpty());
    QVERIFY(info.authentication.isEmpty());
    QVERIFY(info.apVendor.isEmpty());
    QVERIFY(!info.isConnected);
}

void NetworkDiagnosticTypesTests::wifiNetworkInfo_valueSemantics() {
    WiFiNetworkInfo original;
    original.ssid = QStringLiteral("TestNet");
    original.bssid = QStringLiteral("AA:BB:CC:DD:EE:FF");
    original.rssiDbm = -65;
    original.signalQuality = 70;
    original.channelFrequencyKHz = 2'412'000;
    original.channelNumber = 1;
    original.band = QStringLiteral("2.4 GHz");
    original.authentication = QStringLiteral("WPA3");
    original.isConnected = true;

    WiFiNetworkInfo copy = original;
    QCOMPARE(copy.ssid, QStringLiteral("TestNet"));
    QCOMPARE(copy.rssiDbm, -65);
    QCOMPARE(copy.channelNumber, 1);
    QCOMPARE(copy.isConnected, true);
}

void NetworkDiagnosticTypesTests::wifiChannelUtil_defaultConstruction() {
    WiFiChannelUtilization util;

    QCOMPARE(util.channelNumber, 0);
    QVERIFY(util.band.isEmpty());
    QCOMPARE(util.networkCount, 0);
    QVERIFY(util.ssids.isEmpty());
    QCOMPARE(util.averageSignalDbm, 0.0);
    QCOMPARE(util.interferenceScore, 0.0);
}

// ════════════════════════════════════════════════════════════════════════════
// ConnectionInfo
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::connectionInfo_defaultConstruction() {
    ConnectionInfo info;

    QCOMPARE(info.protocol, ConnectionInfo::Protocol::TCP);
    QVERIFY(info.localAddress.isEmpty());
    QCOMPARE(info.localPort, static_cast<uint16_t>(0));
    QVERIFY(info.remoteAddress.isEmpty());
    QCOMPARE(info.remotePort, static_cast<uint16_t>(0));
    QVERIFY(info.state.isEmpty());
    QVERIFY(info.processName.isEmpty());
    QCOMPARE(info.processId, 0u);
}

void NetworkDiagnosticTypesTests::connectionInfo_protocolEnum() {
    QVERIFY(ConnectionInfo::Protocol::TCP != ConnectionInfo::Protocol::UDP);

    ConnectionInfo info;
    info.protocol = ConnectionInfo::Protocol::UDP;
    QCOMPARE(info.protocol, ConnectionInfo::Protocol::UDP);
}

// ════════════════════════════════════════════════════════════════════════════
// FirewallRule
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::firewallRule_defaultConstruction() {
    FirewallRule rule;

    QVERIFY(rule.name.isEmpty());
    QVERIFY(rule.description.isEmpty());
    QVERIFY(!rule.enabled);
    QCOMPARE(rule.direction, FirewallRule::Direction::Inbound);
    QCOMPARE(rule.action, FirewallRule::Action::Allow);
    QCOMPARE(rule.protocol, FirewallRule::Protocol::Any);
    QVERIFY(rule.localPorts.isEmpty());
    QVERIFY(rule.remotePorts.isEmpty());
    QVERIFY(rule.localAddresses.isEmpty());
    QVERIFY(rule.remoteAddresses.isEmpty());
    QVERIFY(rule.applicationPath.isEmpty());
}

void NetworkDiagnosticTypesTests::firewallRule_directionEnum() {
    QVERIFY(FirewallRule::Direction::Inbound != FirewallRule::Direction::Outbound);
}

void NetworkDiagnosticTypesTests::firewallRule_actionEnum() {
    QVERIFY(FirewallRule::Action::Allow != FirewallRule::Action::Block);
}

void NetworkDiagnosticTypesTests::firewallRule_protocolEnum() {
    // All values must be distinct
    const auto tcp = FirewallRule::Protocol::TCP;
    const auto udp = FirewallRule::Protocol::UDP;
    const auto icmpv4 = FirewallRule::Protocol::ICMPv4;
    const auto icmpv6 = FirewallRule::Protocol::ICMPv6;
    const auto any = FirewallRule::Protocol::Any;

    QVERIFY(tcp != udp);
    QVERIFY(udp != icmpv4);
    QVERIFY(icmpv4 != icmpv6);
    QVERIFY(icmpv6 != any);
    QVERIFY(tcp != any);
}

void NetworkDiagnosticTypesTests::firewallRule_profileEnum() {
    QVERIFY(FirewallRule::Profile::Domain != FirewallRule::Profile::Private);
    QVERIFY(FirewallRule::Profile::Private != FirewallRule::Profile::Public);
    // Profile is a bitmask: Domain=1, Private=2, Public=4
    QVERIFY(static_cast<int>(FirewallRule::Profile::Domain) == 1);
    QVERIFY(static_cast<int>(FirewallRule::Profile::Private) == 2);
    QVERIFY(static_cast<int>(FirewallRule::Profile::Public) == 4);
}

// ════════════════════════════════════════════════════════════════════════════
// FirewallConflict / FirewallGap
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::firewallConflict_defaultConstruction() {
    FirewallConflict conflict;

    QVERIFY(conflict.conflictDescription.isEmpty());
    QCOMPARE(conflict.severity, FirewallConflict::Severity::Info);
}

void NetworkDiagnosticTypesTests::firewallConflict_severityEnum() {
    QVERIFY(FirewallConflict::Severity::Critical != FirewallConflict::Severity::Warning);
    QVERIFY(FirewallConflict::Severity::Warning != FirewallConflict::Severity::Info);
}

void NetworkDiagnosticTypesTests::firewallGap_defaultConstruction() {
    FirewallGap gap;

    QVERIFY(gap.description.isEmpty());
    QVERIFY(gap.recommendation.isEmpty());
    QCOMPARE(gap.severity, FirewallGap::Severity::Info);
}

void NetworkDiagnosticTypesTests::firewallGap_severityEnum() {
    QVERIFY(FirewallGap::Severity::Warning != FirewallGap::Severity::Info);
}

// ════════════════════════════════════════════════════════════════════════════
// NetworkShareInfo
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::shareInfo_defaultConstruction() {
    NetworkShareInfo info;

    QVERIFY(info.shareName.isEmpty());
    QVERIFY(info.remark.isEmpty());
    QCOMPARE(info.type, NetworkShareInfo::ShareType::Disk);
    QVERIFY(!info.canRead);
    QVERIFY(!info.canWrite);
}

void NetworkDiagnosticTypesTests::shareInfo_shareTypeEnum() {
    QVERIFY(NetworkShareInfo::ShareType::Disk != NetworkShareInfo::ShareType::Printer);
    QVERIFY(NetworkShareInfo::ShareType::Printer != NetworkShareInfo::ShareType::Device);
    QVERIFY(NetworkShareInfo::ShareType::Device != NetworkShareInfo::ShareType::IPC);
    QVERIFY(NetworkShareInfo::ShareType::IPC != NetworkShareInfo::ShareType::Special);
}

// ════════════════════════════════════════════════════════════════════════════
// Compile-Time Invariants
// ════════════════════════════════════════════════════════════════════════════

void NetworkDiagnosticTypesTests::staticAsserts_defaultConstructible() {
    QVERIFY(std::is_default_constructible_v<NetworkAdapterInfo>);
    QVERIFY(std::is_default_constructible_v<PingReply>);
    QVERIFY(std::is_default_constructible_v<PingResult>);
    QVERIFY(std::is_default_constructible_v<TracerouteHop>);
    QVERIFY(std::is_default_constructible_v<TracerouteResult>);
    QVERIFY(std::is_default_constructible_v<MtrHopStats>);
    QVERIFY(std::is_default_constructible_v<MtrResult>);
    QVERIFY(std::is_default_constructible_v<DnsQueryResult>);
    QVERIFY(std::is_default_constructible_v<DnsServerComparison>);
    QVERIFY(std::is_default_constructible_v<PortScanResult>);
    QVERIFY(std::is_default_constructible_v<PortPreset>);
    QVERIFY(std::is_default_constructible_v<BandwidthTestResult>);
    QVERIFY(std::is_default_constructible_v<BandwidthTestResult::IntervalData>);
    QVERIFY(std::is_default_constructible_v<WiFiNetworkInfo>);
    QVERIFY(std::is_default_constructible_v<WiFiChannelUtilization>);
    QVERIFY(std::is_default_constructible_v<ConnectionInfo>);
    QVERIFY(std::is_default_constructible_v<FirewallRule>);
    QVERIFY(std::is_default_constructible_v<FirewallConflict>);
    QVERIFY(std::is_default_constructible_v<FirewallGap>);
    QVERIFY(std::is_default_constructible_v<NetworkShareInfo>);
}

void NetworkDiagnosticTypesTests::staticAsserts_copyConstructible() {
    QVERIFY(std::is_copy_constructible_v<NetworkAdapterInfo>);
    QVERIFY(std::is_copy_constructible_v<PingReply>);
    QVERIFY(std::is_copy_constructible_v<PingResult>);
    QVERIFY(std::is_copy_constructible_v<TracerouteHop>);
    QVERIFY(std::is_copy_constructible_v<TracerouteResult>);
    QVERIFY(std::is_copy_constructible_v<MtrHopStats>);
    QVERIFY(std::is_copy_constructible_v<MtrResult>);
    QVERIFY(std::is_copy_constructible_v<DnsQueryResult>);
    QVERIFY(std::is_copy_constructible_v<DnsServerComparison>);
    QVERIFY(std::is_copy_constructible_v<PortScanResult>);
    QVERIFY(std::is_copy_constructible_v<PortPreset>);
    QVERIFY(std::is_copy_constructible_v<BandwidthTestResult>);
    QVERIFY(std::is_copy_constructible_v<WiFiNetworkInfo>);
    QVERIFY(std::is_copy_constructible_v<WiFiChannelUtilization>);
    QVERIFY(std::is_copy_constructible_v<ConnectionInfo>);
    QVERIFY(std::is_copy_constructible_v<FirewallRule>);
    QVERIFY(std::is_copy_constructible_v<FirewallConflict>);
    QVERIFY(std::is_copy_constructible_v<FirewallGap>);
    QVERIFY(std::is_copy_constructible_v<NetworkShareInfo>);
}

void NetworkDiagnosticTypesTests::staticAsserts_movable() {
    QVERIFY(std::is_move_constructible_v<NetworkAdapterInfo>);
    QVERIFY(std::is_move_constructible_v<PingReply>);
    QVERIFY(std::is_move_constructible_v<PingResult>);
    QVERIFY(std::is_move_constructible_v<TracerouteHop>);
    QVERIFY(std::is_move_constructible_v<TracerouteResult>);
    QVERIFY(std::is_move_constructible_v<MtrHopStats>);
    QVERIFY(std::is_move_constructible_v<MtrResult>);
    QVERIFY(std::is_move_constructible_v<DnsQueryResult>);
    QVERIFY(std::is_move_constructible_v<DnsServerComparison>);
    QVERIFY(std::is_move_constructible_v<PortScanResult>);
    QVERIFY(std::is_move_constructible_v<PortPreset>);
    QVERIFY(std::is_move_constructible_v<BandwidthTestResult>);
    QVERIFY(std::is_move_constructible_v<WiFiNetworkInfo>);
    QVERIFY(std::is_move_constructible_v<WiFiChannelUtilization>);
    QVERIFY(std::is_move_constructible_v<ConnectionInfo>);
    QVERIFY(std::is_move_constructible_v<FirewallRule>);
    QVERIFY(std::is_move_constructible_v<FirewallConflict>);
    QVERIFY(std::is_move_constructible_v<FirewallGap>);
    QVERIFY(std::is_move_constructible_v<NetworkShareInfo>);
}

QTEST_GUILESS_MAIN(NetworkDiagnosticTypesTests)
#include "test_network_diagnostic_types.moc"
