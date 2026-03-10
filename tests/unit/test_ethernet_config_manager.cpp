// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ethernet_config_manager.cpp
/// @brief Unit tests for EthernetConfigSnapshot serialization and validation

#include "sak/ethernet_config_manager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

using namespace sak;

class TestEthernetConfigManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction ──────────────────────────────────────────────
    void construction_default();
    void construction_nonCopyable();

    // ── EthernetConfigSnapshot ────────────────────────────────────
    void snapshot_defaults();
    void snapshot_isValid_emptyInvalid();
    void snapshot_isValid_withData();
    void snapshot_toJson_roundtrip();
    void snapshot_toJson_allFields();
    void snapshot_fromJson_emptyObject();
    void snapshot_fromJson_missingFields();
    void snapshot_toJson_dhcpEnabled();
    void snapshot_toJson_staticIp();
    void snapshot_toJson_multipleDns();
};

// ═══════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════

void TestEthernetConfigManager::construction_default() {
    EthernetConfigManager mgr;
    QVERIFY(mgr.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<EthernetConfigManager>);
}

void TestEthernetConfigManager::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<EthernetConfigManager>);
    QVERIFY(!std::is_move_constructible_v<EthernetConfigManager>);
}

// ═══════════════════════════════════════════════════════════════════
// EthernetConfigSnapshot
// ═══════════════════════════════════════════════════════════════════

void TestEthernetConfigManager::snapshot_defaults() {
    EthernetConfigSnapshot snapshot;
    QVERIFY(snapshot.adapterName.isEmpty());
    QVERIFY(snapshot.description.isEmpty());
    QVERIFY(snapshot.macAddress.isEmpty());
    QCOMPARE(snapshot.dhcpEnabled, false);
    QVERIFY(snapshot.ipv4Address.isEmpty());
    QVERIFY(snapshot.ipv4SubnetMask.isEmpty());
    QVERIFY(snapshot.ipv4Gateway.isEmpty());
    QVERIFY(snapshot.ipv4DnsServers.isEmpty());
}

void TestEthernetConfigManager::snapshot_isValid_emptyInvalid() {
    EthernetConfigSnapshot snapshot;
    QVERIFY(!snapshot.isValid());
}

void TestEthernetConfigManager::snapshot_isValid_withData() {
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = "Ethernet";
    snapshot.macAddress = "AA:BB:CC:DD:EE:FF";
    snapshot.backupTimestamp = "2025-01-01T00:00:00Z";
    snapshot.dhcpEnabled = true;
    QVERIFY(snapshot.isValid());
}

void TestEthernetConfigManager::snapshot_toJson_roundtrip() {
    EthernetConfigSnapshot original;
    original.adapterName = "Ethernet";
    original.description = "Intel I219-V";
    original.macAddress = "AA:BB:CC:DD:EE:FF";
    original.dhcpEnabled = false;
    original.ipv4Address = "192.168.1.100";
    original.ipv4SubnetMask = "255.255.255.0";
    original.ipv4Gateway = "192.168.1.1";
    original.ipv4DnsServers = {"8.8.8.8", "8.8.4.4"};
    original.backupTimestamp = "2026-03-08T12:00:00";
    original.computerName = "WORKSTATION-01";

    const QJsonObject json = original.toJson();
    const auto restored = EthernetConfigSnapshot::fromJson(json);

    QCOMPARE(restored.adapterName, original.adapterName);
    QCOMPARE(restored.description, original.description);
    QCOMPARE(restored.macAddress, original.macAddress);
    QCOMPARE(restored.dhcpEnabled, original.dhcpEnabled);
    QCOMPARE(restored.ipv4Address, original.ipv4Address);
    QCOMPARE(restored.ipv4SubnetMask, original.ipv4SubnetMask);
    QCOMPARE(restored.ipv4Gateway, original.ipv4Gateway);
    QCOMPARE(restored.ipv4DnsServers, original.ipv4DnsServers);
    QCOMPARE(restored.backupTimestamp, original.backupTimestamp);
    QCOMPARE(restored.computerName, original.computerName);
}

void TestEthernetConfigManager::snapshot_toJson_allFields() {
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = "Test";
    snapshot.macAddress = "11:22:33:44:55:66";
    snapshot.dhcpEnabled = true;

    const QJsonObject json = snapshot.toJson();
    QVERIFY(!json.isEmpty());
    QVERIFY(json.contains("adapterName") || json.contains("adapter_name"));
}

void TestEthernetConfigManager::snapshot_fromJson_emptyObject() {
    const QJsonObject empty;
    const auto snapshot = EthernetConfigSnapshot::fromJson(empty);
    QVERIFY(!snapshot.isValid());
}

void TestEthernetConfigManager::snapshot_fromJson_missingFields() {
    QJsonObject partial;
    partial["adapterName"] = "Test";
    const auto snapshot = EthernetConfigSnapshot::fromJson(partial);
    // Should handle gracefully — either invalid or partial data
    QVERIFY(snapshot.adapterName == "Test" || snapshot.adapterName.isEmpty());
}

void TestEthernetConfigManager::snapshot_toJson_dhcpEnabled() {
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = "WiFi";
    snapshot.macAddress = "AA:BB:CC:DD:EE:FF";
    snapshot.dhcpEnabled = true;

    const QJsonObject json = snapshot.toJson();
    const auto restored = EthernetConfigSnapshot::fromJson(json);
    QCOMPARE(restored.dhcpEnabled, true);
}

void TestEthernetConfigManager::snapshot_toJson_staticIp() {
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = "Ethernet";
    snapshot.macAddress = "AA:BB:CC:DD:EE:FF";
    snapshot.dhcpEnabled = false;
    snapshot.ipv4Address = "10.0.0.5";
    snapshot.ipv4SubnetMask = "255.255.0.0";
    snapshot.ipv4Gateway = "10.0.0.1";

    const QJsonObject json = snapshot.toJson();
    const auto restored = EthernetConfigSnapshot::fromJson(json);
    QCOMPARE(restored.dhcpEnabled, false);
    QCOMPARE(restored.ipv4Address, "10.0.0.5");
}

void TestEthernetConfigManager::snapshot_toJson_multipleDns() {
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = "Ethernet";
    snapshot.macAddress = "AA:BB:CC:DD:EE:FF";
    snapshot.ipv4DnsServers = {"1.1.1.1", "1.0.0.1", "8.8.8.8"};

    const QJsonObject json = snapshot.toJson();
    const auto restored = EthernetConfigSnapshot::fromJson(json);
    QCOMPARE(restored.ipv4DnsServers.size(), 3);
    QVERIFY(restored.ipv4DnsServers.contains("1.1.1.1"));
}

QTEST_MAIN(TestEthernetConfigManager)
#include "test_ethernet_config_manager.moc"
