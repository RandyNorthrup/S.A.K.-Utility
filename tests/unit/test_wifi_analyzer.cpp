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
    // ── Construction ──────────────────────────────────────────────
    void construction_default();
    void construction_nonCopyable();

    // ── frequencyToChannel ────────────────────────────────────────
    void freqToChannel_2_4ghz_channel1();
    void freqToChannel_2_4ghz_channel6();
    void freqToChannel_2_4ghz_channel11();
    void freqToChannel_2_4ghz_channel13();
    void freqToChannel_5ghz_channel36();
    void freqToChannel_5ghz_channel44();
    void freqToChannel_5ghz_channel149();
    void freqToChannel_5ghz_channel165();

    // ── frequencyToBand ───────────────────────────────────────────
    void freqToBand_2_4ghz();
    void freqToBand_5ghz();

    // ── calculateChannelUtilization ───────────────────────────────
    void channelUtil_emptyInput();
    void channelUtil_singleNetwork();
    void channelUtil_multipleNetworks();
};

// ═══════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════

void TestWiFiAnalyzer::construction_default() {
    WiFiAnalyzer analyzer;
    QVERIFY(analyzer.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<WiFiAnalyzer>);
}

void TestWiFiAnalyzer::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<WiFiAnalyzer>);
    QVERIFY(!std::is_move_constructible_v<WiFiAnalyzer>);
}

// ═══════════════════════════════════════════════════════════════════
// frequencyToChannel
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
// frequencyToBand
// ═══════════════════════════════════════════════════════════════════

void TestWiFiAnalyzer::freqToBand_2_4ghz() {
    const auto band = WiFiAnalyzer::frequencyToBand(2'437'000);
    QVERIFY(!band.isEmpty());
    QVERIFY(band.contains("2.4", Qt::CaseInsensitive));
}

void TestWiFiAnalyzer::freqToBand_5ghz() {
    const auto band = WiFiAnalyzer::frequencyToBand(5'180'000);
    QVERIFY(!band.isEmpty());
    QVERIFY(band.contains("5", Qt::CaseInsensitive));
}

// ═══════════════════════════════════════════════════════════════════
// calculateChannelUtilization
// ═══════════════════════════════════════════════════════════════════

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
    QVERIFY(!result.isEmpty());

    bool found_channel_6 = false;
    for (const auto& utilization : result) {
        if (utilization.channelNumber == 6) {
            found_channel_6 = true;
            QVERIFY(utilization.networkCount >= 1);
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
    QVERIFY(!result.isEmpty());

    for (const auto& utilization : result) {
        if (utilization.channelNumber == 6) {
            QVERIFY(utilization.networkCount >= 5);
            break;
        }
    }
}

QTEST_MAIN(TestWiFiAnalyzer)
#include "test_wifi_analyzer.moc"
