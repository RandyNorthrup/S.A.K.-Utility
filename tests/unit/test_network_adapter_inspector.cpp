// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_adapter_inspector.cpp
/// @brief Unit tests for NetworkAdapterInspector static helpers

#include "sak/network_adapter_inspector.h"

#include <QtTest/QtTest>

using namespace sak;

class TestNetworkAdapterInspector : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------
    void construction_default();
    void construction_nonCopyable();

    // -- formatLinkSpeed -------------------------------------------
    void formatLinkSpeed_zero();
    void formatLinkSpeed_bitsPerSecond();
    void formatLinkSpeed_kilobits();
    void formatLinkSpeed_megabits();
    void formatLinkSpeed_gigabit();
    void formatLinkSpeed_tenGigabit();
    void formatLinkSpeed_hundredMegabit();

    // -- formatMacAddress ------------------------------------------
    void formatMacAddress_standard();
    void formatMacAddress_allZeros();
    void formatMacAddress_allOnes();
    void formatMacAddress_zeroLength();
};

// ===================================================================
// Construction
// ===================================================================

void TestNetworkAdapterInspector::construction_default() {
    NetworkAdapterInspector inspector;
    // A default-constructed inspector must not spuriously self-name (the ctor is
    // ": QObject(parent) {}" -- no setObjectName), so objectName() is empty. The prior
    // assertion here was "!isNull() || isNull()", a tautology that could never fail.
    QVERIFY(inspector.objectName().isEmpty());
    QVERIFY(inspector.parent() == nullptr);
}

void TestNetworkAdapterInspector::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<NetworkAdapterInspector>);
    QVERIFY(!std::is_copy_assignable_v<NetworkAdapterInspector>);
}

// ===================================================================
// formatLinkSpeed
// ===================================================================

void TestNetworkAdapterInspector::formatLinkSpeed_zero() {
    const auto result = NetworkAdapterInspector::formatLinkSpeed(0);
    QVERIFY(!result.isEmpty());
    QCOMPARE(result, QStringLiteral("N/A"));
}

void TestNetworkAdapterInspector::formatLinkSpeed_bitsPerSecond() {
    const auto result = NetworkAdapterInspector::formatLinkSpeed(500);
    QVERIFY(!result.isEmpty());
    // 500 bps -> 0.5 Kbps, rendered at 'f' precision 0 (rounds to 1).
    QCOMPARE(result, QStringLiteral("1 Kbps"));
}

void TestNetworkAdapterInspector::formatLinkSpeed_kilobits() {
    const auto result = NetworkAdapterInspector::formatLinkSpeed(56'000);
    QVERIFY(!result.isEmpty());
    QCOMPARE(result, QStringLiteral("56 Kbps"));
}

void TestNetworkAdapterInspector::formatLinkSpeed_megabits() {
    const auto result = NetworkAdapterInspector::formatLinkSpeed(10'000'000);
    QVERIFY(!result.isEmpty());
    QCOMPARE(result, QStringLiteral("10 Mbps"));
}

void TestNetworkAdapterInspector::formatLinkSpeed_hundredMegabit() {
    const auto result = NetworkAdapterInspector::formatLinkSpeed(100'000'000);
    QVERIFY(!result.isEmpty());
    QCOMPARE(result, QStringLiteral("100 Mbps"));
}

void TestNetworkAdapterInspector::formatLinkSpeed_gigabit() {
    const auto result = NetworkAdapterInspector::formatLinkSpeed(1'000'000'000);
    QVERIFY(!result.isEmpty());
    QCOMPARE(result, QStringLiteral("1 Gbps"));
}

void TestNetworkAdapterInspector::formatLinkSpeed_tenGigabit() {
    const auto result = NetworkAdapterInspector::formatLinkSpeed(10'000'000'000ULL);
    QVERIFY(!result.isEmpty());
    QCOMPARE(result, QStringLiteral("10 Gbps"));
}

// ===================================================================
// formatMacAddress
// ===================================================================

void TestNetworkAdapterInspector::formatMacAddress_standard() {
    const unsigned char mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const auto result = NetworkAdapterInspector::formatMacAddress(mac, 6);
    QVERIFY(!result.isEmpty());
    // Uppercase %02X hex, colon-separated (C-locale, platform-independent).
    QCOMPARE(result, QStringLiteral("AA:BB:CC:DD:EE:FF"));
}

void TestNetworkAdapterInspector::formatMacAddress_allZeros() {
    const unsigned char mac[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto result = NetworkAdapterInspector::formatMacAddress(mac, 6);
    QVERIFY(!result.isEmpty());
    QVERIFY(result.contains("00"));
}

void TestNetworkAdapterInspector::formatMacAddress_allOnes() {
    const unsigned char mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const auto result = NetworkAdapterInspector::formatMacAddress(mac, 6);
    QVERIFY(!result.isEmpty());
    QVERIFY(result.contains("FF", Qt::CaseInsensitive));
}

void TestNetworkAdapterInspector::formatMacAddress_zeroLength() {
    const unsigned char mac[] = {0x00};
    const auto result = NetworkAdapterInspector::formatMacAddress(mac, 0);
    QVERIFY(result.isEmpty());
}

QTEST_MAIN(TestNetworkAdapterInspector)
#include "test_network_adapter_inspector.moc"
