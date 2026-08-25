// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ethernet_config_manager.cpp
/// @brief Unit tests for EthernetConfigSnapshot serialization and validation

#include "sak/ethernet_config_manager.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;

class TestEthernetConfigManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------
    void construction_default();
    void construction_nonCopyable();

    // -- EthernetConfigSnapshot ------------------------------------
    void snapshot_defaults();
    void snapshot_isValid_emptyInvalid();
    void snapshot_isValid_rejectsMalformedIpv4();
    void snapshot_isValid_withData();
    void snapshot_toJson_roundtrip();
    void snapshot_toJson_allFields();
    void snapshot_fromJson_emptyObject();
    void snapshot_fromJson_missingFields();
    void snapshot_toJson_dhcpEnabled();
    void snapshot_toJson_staticIp();
    void snapshot_toJson_multipleDns();

    // -- File I/O (atomic write) -----------------------------------
    void saveToFile_atomicRoundTrip();

    // -- MAC lookup (B9-19) ----------------------------------------
    void lookupAdapterMac_unknownAdapterEmpty();
    void lookupAdapterMac_realAdapterHasMacFormat();
};

// ===================================================================
// Construction
// ===================================================================

void TestEthernetConfigManager::construction_default() {
    EthernetConfigManager mgr;
    QCOMPARE(mgr.parent(), static_cast<QObject*>(nullptr));
    // The default-parent case above cannot see whether the ctor
    // (ethernet_config_manager.cpp:127 `: QObject(parent)`) actually FORWARDS its argument --
    // a ctor that DISCARDED parent reports nullptr here too. Pin the parent edge with a real
    // owner: it is the ownership/thread-affinity contract declared at
    // ethernet_config_manager.h:54 and relied on by network_diagnostic_controller.cpp:97
    // (`std::make_unique<EthernetConfigManager>(this)`).
    QObject owner;
    EthernetConfigManager child(&owner);
    QCOMPARE(child.parent(), &owner);
    QVERIFY(owner.children().contains(&child));
    // The is_copy_constructible_v check that stood here is a compile-time constant already
    // enforced by the static_assert at ethernet_config_manager.h:147-148 and asserted verbatim
    // in construction_nonCopyable() below; it can never fail at runtime, so it is not repeated.
}

void TestEthernetConfigManager::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<EthernetConfigManager>);
    QVERIFY(!std::is_move_constructible_v<EthernetConfigManager>);
}

// ===================================================================
// EthernetConfigSnapshot
// ===================================================================

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
    // Metadata defaults are load-bearing, not cosmetic: isValid() refuses on an empty
    // backupTimestamp (ethernet_config_manager.cpp:111), so a non-empty default would
    // silently disarm that guard for every directly-constructed snapshot (e.g. the one
    // setDnsServers builds at :418-420). Pin all 10 members so this stays exhaustive.
    QVERIFY(snapshot.backupTimestamp.isEmpty());
    QVERIFY(snapshot.computerName.isEmpty());
}

void TestEthernetConfigManager::snapshot_isValid_emptyInvalid() {
    EthernetConfigSnapshot snapshot;
    QVERIFY(!snapshot.isValid());

    // isValid() is a three-guard refuser (ethernet_config_manager.cpp:110-123) whose third
    // guard delegates to the four-arm ipv4FieldsWellFormed (:44-60). A default snapshot trips
    // guard 1 alone, so isolate every guard from a known-valid base by breaking one field.
    EthernetConfigSnapshot base;
    base.adapterName = QStringLiteral("Ethernet");
    base.backupTimestamp = QStringLiteral("2026-03-08T12:00:00");
    base.dhcpEnabled = false;
    base.ipv4Address = QStringLiteral("192.168.1.100");
    base.ipv4SubnetMask = QStringLiteral("255.255.255.0");
    base.ipv4Gateway = QStringLiteral("192.168.1.1");
    base.ipv4DnsServers = {QStringLiteral("8.8.8.8")};
    QVERIFY(base.isValid());

    // Guard 1 (:111): each identity field refuses on its own.
    {
        auto s = base;
        s.adapterName.clear();
        QVERIFY(!s.isValid());
    }
    {
        auto s = base;
        s.backupTimestamp.clear();
        QVERIFY(!s.isValid());
    }

    // Guard 2 (:117-119): a localized/failed parse recognizes nothing, leaving dhcpEnabled=false
    // AND no address -- that must fail closed rather than persist an empty backup...
    {
        auto s = base;
        s.ipv4Address.clear();
        QVERIFY(!s.isValid());
    }
    // ...while the same empty address IS legitimate on the DHCP path.
    {
        auto s = base;
        s.ipv4Address.clear();
        s.dhcpEnabled = true;
        QVERIFY(s.isValid());
    }
}

void TestEthernetConfigManager::snapshot_isValid_rejectsMalformedIpv4() {
    EthernetConfigSnapshot base;
    base.adapterName = QStringLiteral("Ethernet");
    base.backupTimestamp = QStringLiteral("2026-03-08T12:00:00");
    base.dhcpEnabled = false;
    base.ipv4Address = QStringLiteral("192.168.1.100");
    base.ipv4SubnetMask = QStringLiteral("255.255.255.0");
    base.ipv4Gateway = QStringLiteral("192.168.1.1");
    base.ipv4DnsServers = {QStringLiteral("8.8.8.8")};
    QVERIFY(base.isValid());

    // Guard 3 (:122 -> :44-60): every arm refuses a value that is not a dotted-quad IPv4
    // literal, before it can reach `netsh interface ip set address addr=...`. "::1" is the
    // load-bearing case: a bare QHostAddress::setAddress accepts it, the protocol() check
    // at :39 does not.
    {
        auto s = base;
        s.ipv4Address = QStringLiteral("::1");
        QVERIFY(!s.isValid());
    }
    {
        auto s = base;
        s.ipv4SubnetMask = QStringLiteral("not-an-ip");
        QVERIFY(!s.isValid());
    }
    {
        auto s = base;
        s.ipv4Gateway = QStringLiteral("::1");
        QVERIFY(!s.isValid());
    }
    {
        auto s = base;
        s.ipv4DnsServers = {QStringLiteral("8.8.8.8"), QStringLiteral("::1")};
        QVERIFY(!s.isValid());
    }
    // An absent optional gateway / DNS set stays acceptable (:42-43).
    {
        auto s = base;
        s.ipv4Gateway.clear();
        s.ipv4DnsServers.clear();
        QVERIFY(s.isValid());
    }
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
    // Pin the ON-DISK FILE FORMAT, not just its shape: exact key names, exact JSON
    // value types, and which value each key carries. Every field gets a distinct
    // sentinel, so a symmetric key rename in toJson()+fromJson(), or a value swap
    // between two same-typed string keys, goes red here even though every
    // round-trip test stays green. Backups written by earlier builds are read back
    // by these literal names, so the names are the contract.
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = "Test";
    snapshot.description = "Intel I219-V";
    snapshot.macAddress = "11:22:33:44:55:66";
    snapshot.dhcpEnabled = true;
    snapshot.ipv4Address = "10.1.2.3";
    snapshot.ipv4SubnetMask = "255.255.255.0";
    snapshot.ipv4Gateway = "10.1.2.1";
    snapshot.ipv4DnsServers = {"9.9.9.9", "1.1.1.1"};
    snapshot.backupTimestamp = "2026-03-08T12:00:00";
    snapshot.computerName = "WS-SENTINEL";

    const QJsonObject json = snapshot.toJson();

    QJsonParseError parseError{};
    const QJsonDocument expectedDoc = QJsonDocument::fromJson(R"({
        "adapterName": "Test",
        "description": "Intel I219-V",
        "macAddress": "11:22:33:44:55:66",
        "dhcpEnabled": true,
        "ipv4Address": "10.1.2.3",
        "ipv4SubnetMask": "255.255.255.0",
        "ipv4Gateway": "10.1.2.1",
        "ipv4DnsServers": ["9.9.9.9", "1.1.1.1"],
        "backupTimestamp": "2026-03-08T12:00:00",
        "computerName": "WS-SENTINEL"
    })",
                                                              &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);

    // Deep compare: pins the exact 10 key names (a rename adds/removes a key),
    // the value bound to each key (a swap moves a value), and each value's JSON
    // type (dhcpEnabled must stay a bool, ipv4DnsServers an ordered array).
    QCOMPARE(json, expectedDoc.object());
    QCOMPARE(json.size(), 10);
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
    // fromJson assigns adapterName unconditionally from a present string key; the isEmpty
    // disjunct was unreachable and would mask a broken assignment.
    QCOMPARE(snapshot.adapterName, QStringLiteral("Test"));
    // Every ABSENT key must deserialize to the empty/false default -- fail closed. An invented
    // default (toBool(true), or the live hostname for computerName) would let a truncated
    // backup satisfy the isValid() guard at ethernet_config_manager.cpp:117 and reach a live
    // netsh apply as a DHCP switch, or fabricate provenance the backup never recorded.
    QVERIFY(snapshot.description.isEmpty());
    QVERIFY(snapshot.macAddress.isEmpty());
    QCOMPARE(snapshot.dhcpEnabled, false);
    QVERIFY(snapshot.ipv4Address.isEmpty());
    QVERIFY(snapshot.ipv4SubnetMask.isEmpty());
    QVERIFY(snapshot.ipv4Gateway.isEmpty());
    QVERIFY(snapshot.ipv4DnsServers.isEmpty());
    QVERIFY(snapshot.backupTimestamp.isEmpty());
    QVERIFY(snapshot.computerName.isEmpty());
    // ...and with no dhcp flag and no address recorded, the truncated snapshot is unrestorable.
    QVERIFY(!snapshot.isValid());
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
    // The JSON array round-trip preserves order, so pin the full ordered list.
    QCOMPARE(restored.ipv4DnsServers, (QStringList{"1.1.1.1", "1.0.0.1", "8.8.8.8"}));
}

void TestEthernetConfigManager::saveToFile_atomicRoundTrip() {
    // B9-12: saveToFile writes atomically (QSaveFile) and round-trips through
    // loadFromFile; overwriting an existing backup must fully replace it rather
    // than truncate-then-write.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("eth.json"));

    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = "Ethernet";
    snapshot.dhcpEnabled = false;
    snapshot.ipv4Address = "192.168.1.100";
    snapshot.ipv4SubnetMask = "255.255.255.0";
    snapshot.ipv4Gateway = "192.168.1.1";
    snapshot.ipv4DnsServers = {"8.8.8.8"};
    snapshot.backupTimestamp = "2026-03-08T12:00:00";
    snapshot.computerName = "WS-01";
    QVERIFY(snapshot.isValid());

    EthernetConfigManager mgr;
    QVERIFY(mgr.saveToFile(snapshot, path));
    QVERIFY(QFileInfo::exists(path));

    // First round trip pins ALL TEN serialized fields through the file path, not just three.
    const auto first = mgr.loadFromFile(path);
    QVERIFY(first.isValid());
    QCOMPARE(first.adapterName, snapshot.adapterName);
    QCOMPARE(first.description, snapshot.description);
    QCOMPARE(first.macAddress, snapshot.macAddress);
    QCOMPARE(first.dhcpEnabled, snapshot.dhcpEnabled);
    QCOMPARE(first.ipv4Address, snapshot.ipv4Address);
    QCOMPARE(first.ipv4SubnetMask, snapshot.ipv4SubnetMask);
    QCOMPARE(first.ipv4Gateway, snapshot.ipv4Gateway);
    QCOMPARE(first.ipv4DnsServers, snapshot.ipv4DnsServers);
    QCOMPARE(first.backupTimestamp, snapshot.backupTimestamp);
    QCOMPARE(first.computerName, snapshot.computerName);

    // Overwrite with MATERIALLY DIFFERENT content so "the write happened" is observable:
    // re-saving the same bytes cannot tell a real replace from a skipped write. This second
    // snapshot is the DHCP shape, so its IPv4 fields are legitimately empty and isValid()
    // still holds via the dhcpEnabled branch (ethernet_config_manager.cpp:117).
    EthernetConfigSnapshot rewritten;
    rewritten.adapterName = "Wi-Fi";
    rewritten.description = "Realtek GbE";
    rewritten.macAddress = "AA:BB:CC:DD:EE:FF";
    rewritten.dhcpEnabled = true;
    rewritten.backupTimestamp = "2026-03-09T13:00:00";
    rewritten.computerName = "WS-02";
    QVERIFY(rewritten.isValid());
    QVERIFY(mgr.saveToFile(rewritten, path));

    // An atomic writer FULLY replaces the prior file: the reload is the new snapshot in
    // every field, and no value from the first backup survives.
    const auto restored = mgr.loadFromFile(path);
    QVERIFY(restored.isValid());
    QCOMPARE(restored.adapterName, QStringLiteral("Wi-Fi"));
    QCOMPARE(restored.description, QStringLiteral("Realtek GbE"));
    QCOMPARE(restored.macAddress, QStringLiteral("AA:BB:CC:DD:EE:FF"));
    QCOMPARE(restored.dhcpEnabled, true);
    QCOMPARE(restored.backupTimestamp, QStringLiteral("2026-03-09T13:00:00"));
    QCOMPARE(restored.computerName, QStringLiteral("WS-02"));
    // Stale static-IP values from the FIRST backup must be gone, not merged or appended.
    QVERIFY(restored.ipv4Address.isEmpty());
    QVERIFY(restored.ipv4SubnetMask.isEmpty());
    QVERIFY(restored.ipv4Gateway.isEmpty());
    QCOMPARE(restored.ipv4DnsServers, (QStringList{}));
}

void TestEthernetConfigManager::lookupAdapterMac_unknownAdapterEmpty() {
    // B9-19: a nonexistent adapter name resolves to an empty MAC (no match), and an
    // empty name short-circuits to empty.
    QVERIFY(EthernetConfigManager::lookupAdapterMac(QString()).isEmpty());
    QVERIFY(
        EthernetConfigManager::lookupAdapterMac(QStringLiteral("__no_such_adapter__")).isEmpty());
}

void TestEthernetConfigManager::lookupAdapterMac_realAdapterHasMacFormat() {
    // If the host exposes any interface with a hardware address, the lookup must return
    // it in canonical MAC form. Skips cleanly on a host with no such interface.
    QString anyName;
    QString anySystemName;
    QString expectedMac;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (!iface.hardwareAddress().isEmpty() && !iface.humanReadableName().isEmpty()) {
            anyName = iface.humanReadableName();
            anySystemName = iface.name();
            expectedMac = iface.hardwareAddress();
            break;
        }
    }
    if (anyName.isEmpty()) {
        QSKIP("No interface with a hardware address on this host");
    }
    const QString mac = EthernetConfigManager::lookupAdapterMac(anyName);
    QCOMPARE(mac, expectedMac);
    // Canonical form: six colon-separated hex octets.
    const QRegularExpression macRe(QStringLiteral("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$"));
    QVERIFY(macRe.match(mac).hasMatch());
    // The matcher accepts the SYSTEM name as well as the friendly name
    // (ethernet_config_manager.cpp:134; contract at ethernet_config_manager.h:67-68).
    // On Windows the system name is the adapter GUID -- the form a win32/netsh caller
    // supplies -- so pin the second disjunct: dropping it makes this lookup return empty.
    QVERIFY(!anySystemName.isEmpty());
    QCOMPARE(EthernetConfigManager::lookupAdapterMac(anySystemName), expectedMac);
}

QTEST_MAIN(TestEthernetConfigManager)
#include "test_ethernet_config_manager.moc"
