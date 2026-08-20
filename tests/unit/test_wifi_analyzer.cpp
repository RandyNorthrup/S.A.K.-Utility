// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_wifi_analyzer.cpp
/// @brief Unit tests for WiFiAnalyzer static helpers and channel utilities

#include "sak/wifi_analyzer.h"

#include <QtTest/QtTest>

using namespace sak;

class TestWiFiAnalyzer : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------
    void construction_default();
    void construction_nonCopyable();

    // -- frequencyToChannel ----------------------------------------
    void freqToChannel_2_4ghz_channel1();
    void freqToChannel_2_4ghz_channel6();
    void freqToChannel_2_4ghz_channel11();
    void freqToChannel_2_4ghz_channel13();
    void freqToChannel_5ghz_channel36();
    void freqToChannel_5ghz_channel44();
    void freqToChannel_5ghz_channel149();
    void freqToChannel_5ghz_channel165();

    // -- frequencyToBand -------------------------------------------
    void freqToBand_2_4ghz();
    void freqToBand_5ghz();

    // -- calculateChannelUtilization -------------------------------
    void channelUtil_emptyInput();
    void channelUtil_singleNetwork();
    void channelUtil_multipleNetworks();

    // -- deriveBssSecurity (per-BSSID, B9-13) ----------------------
    void bssSecurity_openNoPrivacyNoIe();
    void bssSecurity_wepPrivacyNoIe();
    void bssSecurity_wpa1VendorIe();
    void bssSecurity_wpa2RsnPsk();
    void bssSecurity_wpa3RsnSae();
    void bssSecurity_evilTwinOpenStaysInsecure();
    void bssSecurity_truncatedIeNoCrash();
    void bssSecurity_emptyRsnPayloadNotSecure();
    void bssSecurity_rsnInvalidVersionNotSecure();

    // -- lookupVendor (once-init OUI table, B9-14) -----------------
    void lookupVendor_knownFallbackPrefix();
    void lookupVendor_unknownPrefixEmpty();
    void lookupVendor_shortInputEmpty();
};

// ===================================================================
// Construction
// ===================================================================

void TestWiFiAnalyzer::construction_default() {
    WiFiAnalyzer analyzer;
    QVERIFY(analyzer.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<WiFiAnalyzer>);
}

void TestWiFiAnalyzer::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<WiFiAnalyzer>);
    QVERIFY(!std::is_move_constructible_v<WiFiAnalyzer>);
}

// ===================================================================
// frequencyToChannel
// ===================================================================

void TestWiFiAnalyzer::freqToChannel_2_4ghz_channel1() {
    // 2412 MHz = Channel 1 (2.4 GHz)
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'412'000), 1);
}

void TestWiFiAnalyzer::freqToChannel_2_4ghz_channel6() {
    // 2437 MHz = Channel 6
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'437'000), 6);
}

void TestWiFiAnalyzer::freqToChannel_2_4ghz_channel11() {
    // 2462 MHz = Channel 11
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'462'000), 11);
}

void TestWiFiAnalyzer::freqToChannel_2_4ghz_channel13() {
    // 2472 MHz = Channel 13
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(2'472'000), 13);
}

void TestWiFiAnalyzer::freqToChannel_5ghz_channel36() {
    // 5180 MHz = Channel 36
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'180'000), 36);
}

void TestWiFiAnalyzer::freqToChannel_5ghz_channel44() {
    // 5220 MHz = Channel 44
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'220'000), 44);
}

void TestWiFiAnalyzer::freqToChannel_5ghz_channel149() {
    // 5745 MHz = Channel 149
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'745'000), 149);
}

void TestWiFiAnalyzer::freqToChannel_5ghz_channel165() {
    // 5825 MHz = Channel 165
    QCOMPARE(WiFiAnalyzer::frequencyToChannel(5'825'000), 165);
}

// ===================================================================
// frequencyToBand
// ===================================================================

void TestWiFiAnalyzer::freqToBand_2_4ghz() {
    const auto band = WiFiAnalyzer::frequencyToBand(2'437'000);
    QVERIFY(!band.isEmpty());
    QCOMPARE(band, QStringLiteral("2.4 GHz"));
}

void TestWiFiAnalyzer::freqToBand_5ghz() {
    const auto band = WiFiAnalyzer::frequencyToBand(5'180'000);
    QVERIFY(!band.isEmpty());
    QCOMPARE(band, QStringLiteral("5 GHz"));
}

// ===================================================================
// calculateChannelUtilization
// ===================================================================

void TestWiFiAnalyzer::channelUtil_emptyInput() {
    const QVector<WiFiNetworkInfo> empty;
    const auto result = WiFiAnalyzer::calculateChannelUtilization(empty);
    QVERIFY(result.isEmpty());
}

void TestWiFiAnalyzer::channelUtil_singleNetwork() {
    QVector<WiFiNetworkInfo> networks;
    WiFiNetworkInfo network;
    network.ssid = "TestNetwork";
    network.channelNumber = 6;
    network.channelFrequencyKHz = 2'437'000;
    network.signalQuality = 50;
    networks.append(network);

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    // All networks on channel 6 with empty band collapse to one "|6" key.
    QCOMPARE(result.size(), 1);

    bool found_channel_6 = false;
    for (const auto& utilization : result) {
        if (utilization.channelNumber == 6) {
            found_channel_6 = true;
            QCOMPARE(utilization.networkCount, 1);
        }
    }
    QVERIFY(found_channel_6);
}

void TestWiFiAnalyzer::channelUtil_multipleNetworks() {
    QVector<WiFiNetworkInfo> networks;
    for (int idx = 0; idx < 5; ++idx) {
        WiFiNetworkInfo network;
        network.ssid = QString("Network%1").arg(idx);
        network.channelNumber = 6;
        network.channelFrequencyKHz = 2'437'000;
        network.signalQuality = 60 - idx;
        networks.append(network);
    }

    const auto result = WiFiAnalyzer::calculateChannelUtilization(networks);
    // All 5 networks share channel 6 + empty band -> one merged "|6" entry.
    QCOMPARE(result.size(), 1);

    for (const auto& utilization : result) {
        if (utilization.channelNumber == 6) {
            QCOMPARE(utilization.networkCount, 5);
            break;
        }
    }
}

// ===================================================================
// deriveBssSecurity -- per-BSSID security from 802.11 IEs (B9-13)
// ===================================================================

void TestWiFiAnalyzer::bssSecurity_openNoPrivacyNoIe() {
    // No Privacy bit, no RSN/WPA IE => genuinely open.
    const auto sec = WiFiAnalyzer::deriveBssSecurity(false, nullptr, 0);
    QCOMPARE(sec.authentication, QStringLiteral("Open"));
    QCOMPARE(sec.encryption, QStringLiteral("None"));
    QVERIFY(!sec.isSecure);
}

void TestWiFiAnalyzer::bssSecurity_wepPrivacyNoIe() {
    // Privacy bit set but no RSN/WPA IE => legacy WEP.
    const auto sec = WiFiAnalyzer::deriveBssSecurity(true, nullptr, 0);
    QCOMPARE(sec.authentication, QStringLiteral("WEP"));
    QVERIFY(sec.isSecure);
}

void TestWiFiAnalyzer::bssSecurity_wpa1VendorIe() {
    // Vendor-specific IE (221) with WPA OUI 00:50:F2 type 01 => WPA1.
    const unsigned char ie[] = {221, 4, 0x00, 0x50, 0xF2, 0x01};
    const auto sec = WiFiAnalyzer::deriveBssSecurity(true, ie, sizeof(ie));
    QCOMPARE(sec.authentication, QStringLiteral("WPA"));
    QVERIFY(sec.isSecure);
}

void TestWiFiAnalyzer::bssSecurity_wpa2RsnPsk() {
    // RSN IE (48) with PSK AKM (00:0F:AC:02) => WPA2.
    const unsigned char ie[] = {48,   18,   0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00,
                                0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02};
    const auto sec = WiFiAnalyzer::deriveBssSecurity(true, ie, sizeof(ie));
    QCOMPARE(sec.authentication, QStringLiteral("WPA2"));
    QCOMPARE(sec.encryption, QStringLiteral("AES-CCMP"));
    QVERIFY(sec.isSecure);
}

void TestWiFiAnalyzer::bssSecurity_wpa3RsnSae() {
    // RSN IE (48) with SAE AKM (00:0F:AC:08) => WPA3.
    const unsigned char ie[] = {48,   18,   0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00,
                                0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x08};
    const auto sec = WiFiAnalyzer::deriveBssSecurity(true, ie, sizeof(ie));
    QCOMPARE(sec.authentication, QStringLiteral("WPA3"));
    QVERIFY(sec.isSecure);
}

void TestWiFiAnalyzer::bssSecurity_evilTwinOpenStaysInsecure() {
    // Core of B9-13: an AP with no Privacy bit and no security IE must be reported
    // insecure REGARDLESS of any sibling BSSID advertising the same SSID securely.
    // deriveBssSecurity only sees this AP's own beacon, so it can never inherit a
    // sibling's label.
    const auto sec = WiFiAnalyzer::deriveBssSecurity(false, nullptr, 0);
    QVERIFY(!sec.isSecure);
    QCOMPARE(sec.authentication, QStringLiteral("Open"));
}

void TestWiFiAnalyzer::bssSecurity_truncatedIeNoCrash() {
    // Malformed IE claiming a length past the buffer end must not over-read; the
    // walk bails and falls back to the Privacy bit (here clear => Open).
    const unsigned char ie[] = {48, 50, 0x01, 0x00};
    const auto sec = WiFiAnalyzer::deriveBssSecurity(false, ie, sizeof(ie));
    QVERIFY(!sec.isSecure);
    QCOMPARE(sec.authentication, QStringLiteral("Open"));
}

// A zero-length RSN IE payload must NOT be trusted as WPA2-secure on element-id
// presence alone (codex-review-3). With no Privacy bit it resolves to Open.
void TestWiFiAnalyzer::bssSecurity_emptyRsnPayloadNotSecure() {
    const unsigned char ie[] = {48, 0};  // RSN id, length 0 -> no version field
    const auto sec = WiFiAnalyzer::deriveBssSecurity(false, ie, sizeof(ie));
    QVERIFY(!sec.isSecure);
    QCOMPARE(sec.authentication, QStringLiteral("Open"));
}

// An RSN IE whose version field is not 1 (the only defined version) is garbage
// and must not be labelled WPA2-secure.
void TestWiFiAnalyzer::bssSecurity_rsnInvalidVersionNotSecure() {
    // RSN id, length 6, version=2 (invalid), then a zeroed group-cipher suite.
    const unsigned char ie[] = {48, 6, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto sec = WiFiAnalyzer::deriveBssSecurity(false, ie, sizeof(ie));
    QVERIFY(!sec.isSecure);
    QCOMPARE(sec.authentication, QStringLiteral("Open"));
}

// ===================================================================
// lookupVendor -- once-initialized OUI table (B9-14 thread-safe init)
// ===================================================================

void TestWiFiAnalyzer::lookupVendor_knownFallbackPrefix() {
    // With no oui_database.txt beside the test exe, the built-in fallback set is
    // seeded once. A known fallback OUI must resolve -- proving the once-init
    // table populated.
    QCOMPARE(WiFiAnalyzer::lookupVendor(QStringLiteral("00:50:56:AA:BB:CC")),
             QStringLiteral("VMware"));
}

void TestWiFiAnalyzer::lookupVendor_unknownPrefixEmpty() {
    QVERIFY(WiFiAnalyzer::lookupVendor(QStringLiteral("FE:DC:BA:00:11:22")).isEmpty());
}

void TestWiFiAnalyzer::lookupVendor_shortInputEmpty() {
    QVERIFY(WiFiAnalyzer::lookupVendor(QStringLiteral("00:50")).isEmpty());
}

QTEST_MAIN(TestWiFiAnalyzer)
#include "test_wifi_analyzer.moc"
