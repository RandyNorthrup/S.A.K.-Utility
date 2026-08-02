// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_wifi_profile_scanner.cpp
/// @brief Unit tests for the WLANProfile-XML security-type parser (locale-independent)

#include "sak/wifi_profile_scanner.h"

#include <QtTest/QtTest>

class TestWifiProfileScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── wifiSecurityTypeFromProfileXml ──────────────────────────────────
    void security_wpa2Personal();
    void security_wpa3Personal();
    void security_open();
    void security_enterprise();
    void security_caseInsensitiveTag();
    void security_unknownTokenPassthrough();
    void security_noAuthElement();
    void security_emptyXml();
    void security_localeIndependent();
};

namespace {

/// A minimal but schema-shaped WLANProfile document with the given <authentication> token.
QString profileXml(const QString& auth_token) {
    return QStringLiteral(
               "<?xml version=\"1.0\"?>\r\n"
               "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\r\n"
               "  <name>MyNet</name>\r\n"
               "  <SSIDConfig><SSID><name>MyNet</name></SSID></SSIDConfig>\r\n"
               "  <connectionType>ESS</connectionType>\r\n"
               "  <MSM>\r\n"
               "    <security>\r\n"
               "      <authEncryption>\r\n"
               "        <authentication>%1</authentication>\r\n"
               "        <encryption>AES</encryption>\r\n"
               "        <useOneX>false</useOneX>\r\n"
               "      </authEncryption>\r\n"
               "    </security>\r\n"
               "  </MSM>\r\n"
               "</WLANProfile>\r\n")
        .arg(auth_token);
}

}  // namespace

void TestWifiProfileScanner::security_wpa2Personal() {
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA2PSK"))),
             QStringLiteral("WPA2-Personal"));
}

void TestWifiProfileScanner::security_wpa3Personal() {
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA3SAE"))),
             QStringLiteral("WPA3-Personal"));
}

void TestWifiProfileScanner::security_open() {
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("open"))),
             QStringLiteral("Open"));
}

void TestWifiProfileScanner::security_enterprise() {
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA2"))),
             QStringLiteral("WPA2-Enterprise"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA3ENT192"))),
             QStringLiteral("WPA3-Enterprise (192-bit)"));
}

void TestWifiProfileScanner::security_caseInsensitiveTag() {
    // Element name and token casing must not matter (schema tokens vary in case across exporters).
    const QString xml =
        profileXml(QStringLiteral("wpa2psk"))
            .replace(QStringLiteral("<authentication>"), QStringLiteral("<Authentication>"))
            .replace(QStringLiteral("</authentication>"), QStringLiteral("</Authentication>"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(xml), QStringLiteral("WPA2-Personal"));
}

void TestWifiProfileScanner::security_unknownTokenPassthrough() {
    // A future/unknown scheme is preserved verbatim rather than dropped.
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA4FUTURE"))),
             QStringLiteral("WPA4FUTURE"));
}

void TestWifiProfileScanner::security_noAuthElement() {
    const QString xml = QStringLiteral("<WLANProfile><name>MyNet</name></WLANProfile>");
    QVERIFY(sak::wifiSecurityTypeFromProfileXml(xml).isEmpty());
}

void TestWifiProfileScanner::security_emptyXml() {
    QVERIFY(sak::wifiSecurityTypeFromProfileXml(QString()).isEmpty());
}

void TestWifiProfileScanner::security_localeIndependent() {
    // The whole point of the WLAN-API rewrite: the token is schema text, so a non-English Windows
    // (which localizes netsh CONSOLE output but never the profile XML) still yields the right type.
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPAPSK"))),
             QStringLiteral("WPA-Personal"));
}

QTEST_MAIN(TestWifiProfileScanner)
#include "test_wifi_profile_scanner.moc"
