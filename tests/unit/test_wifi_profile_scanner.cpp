// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_wifi_profile_scanner.cpp
/// @brief Unit tests for WiFi profile parsing utilities

#include <QtTest/QtTest>

#include "sak/wifi_profile_scanner.h"

class TestWifiProfileScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── parseWifiProfileNames ───────────────────────────────────────────
    void parseNames_typicalOutput();
    void parseNames_emptyOutput();
    void parseNames_noProfiles();
    void parseNames_multipleProfiles();
    void parseNames_profileWithSpaces();

    // ── parseWifiSecurityType ───────────────────────────────────────────
    void parseSecurity_wpa2Personal();
    void parseSecurity_open();
    void parseSecurity_noAuthLine();
    void parseSecurity_emptyOutput();
};

// ============================================================================
// parseWifiProfileNames Tests
// ============================================================================

void TestWifiProfileScanner::parseNames_typicalOutput()
{
    const QString output =
        "Profiles on interface Wi-Fi:\r\n"
        "\r\n"
        "Group policy profiles (read only)\r\n"
        "---------------------------------\r\n"
        "    <None>\r\n"
        "\r\n"
        "User profiles\r\n"
        "-------------\r\n"
        "    All User Profile     : MyHomeNetwork\r\n"
        "    All User Profile     : OfficeWifi\r\n";

    const QStringList names = sak::parseWifiProfileNames(output);
    QCOMPARE(names.size(), 2);
    QCOMPARE(names.at(0), QStringLiteral("MyHomeNetwork"));
    QCOMPARE(names.at(1), QStringLiteral("OfficeWifi"));
}

void TestWifiProfileScanner::parseNames_emptyOutput()
{
    const QStringList names = sak::parseWifiProfileNames(QString());
    QVERIFY(names.isEmpty());
}

void TestWifiProfileScanner::parseNames_noProfiles()
{
    const QString output =
        "Profiles on interface Wi-Fi:\r\n"
        "\r\n"
        "Group policy profiles (read only)\r\n"
        "---------------------------------\r\n"
        "    <None>\r\n"
        "\r\n"
        "User profiles\r\n"
        "-------------\r\n"
        "    <None>\r\n";

    const QStringList names = sak::parseWifiProfileNames(output);
    QVERIFY(names.isEmpty());
}

void TestWifiProfileScanner::parseNames_multipleProfiles()
{
    const QString output =
        "    All User Profile     : Network1\r\n"
        "    All User Profile     : Network2\r\n"
        "    All User Profile     : Network3\r\n"
        "    All User Profile     : Network4\r\n"
        "    All User Profile     : Network5\r\n";

    const QStringList names = sak::parseWifiProfileNames(output);
    QCOMPARE(names.size(), 5);
    QCOMPARE(names.at(4), QStringLiteral("Network5"));
}

void TestWifiProfileScanner::parseNames_profileWithSpaces()
{
    const QString output =
        "    All User Profile     : My Home WiFi Network\r\n";

    const QStringList names = sak::parseWifiProfileNames(output);
    QCOMPARE(names.size(), 1);
    QCOMPARE(names.at(0), QStringLiteral("My Home WiFi Network"));
}

// ============================================================================
// parseWifiSecurityType Tests
// ============================================================================

void TestWifiProfileScanner::parseSecurity_wpa2Personal()
{
    const QString detail =
        "Profile MyNetwork on interface Wi-Fi:\r\n"
        "=======================================================================\r\n"
        "\r\n"
        "Applied: All User Profile\r\n"
        "\r\n"
        "Profile information\r\n"
        "-------------------\r\n"
        "    Version                : 1\r\n"
        "    Type                   : Wireless LAN\r\n"
        "    Name                   : MyNetwork\r\n"
        "    Control options        :\r\n"
        "        Connection mode    : Connect automatically\r\n"
        "\r\n"
        "Connectivity settings\r\n"
        "---------------------\r\n"
        "    Number of SSIDs        : 1\r\n"
        "    SSID name              : \"MyNetwork\"\r\n"
        "    Network type           : Infrastructure\r\n"
        "    Radio type             : [ Any Radio Type ]\r\n"
        "\r\n"
        "Security settings\r\n"
        "-----------------\r\n"
        "    Authentication         : WPA2-Personal\r\n"
        "    Cipher                 : CCMP\r\n"
        "    Security key           : Present\r\n";

    const QString security_type = sak::parseWifiSecurityType(detail);
    QCOMPARE(security_type, QStringLiteral("WPA2-Personal"));
}

void TestWifiProfileScanner::parseSecurity_open()
{
    const QString detail =
        "Security settings\r\n"
        "-----------------\r\n"
        "    Authentication         : Open\r\n"
        "    Cipher                 : None\r\n";

    const QString security_type = sak::parseWifiSecurityType(detail);
    QCOMPARE(security_type, QStringLiteral("Open"));
}

void TestWifiProfileScanner::parseSecurity_noAuthLine()
{
    const QString detail =
        "Profile information\r\n"
        "-------------------\r\n"
        "    Version                : 1\r\n"
        "    Type                   : Wireless LAN\r\n";

    const QString security_type = sak::parseWifiSecurityType(detail);
    QVERIFY(security_type.isEmpty());
}

void TestWifiProfileScanner::parseSecurity_emptyOutput()
{
    const QString security_type = sak::parseWifiSecurityType(QString());
    QVERIFY(security_type.isEmpty());
}

QTEST_MAIN(TestWifiProfileScanner)
#include "test_wifi_profile_scanner.moc"
