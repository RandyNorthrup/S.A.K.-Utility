// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_wifi_profile_scanner.cpp
/// @brief Unit tests for the WLANProfile-XML security-type parser (locale-independent)

#include "sak/wifi_profile_scanner.h"

#include <QtTest/QtTest>

class TestWifiProfileScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- wifiSecurityTypeFromProfileXml ----------------------------------
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
    // The rest of the non-WPA rows: a dropped/renamed table row does not fail, it passes the raw
    // schema token through verbatim, which sampling only "open" would never notice.
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("shared"))),
             QStringLiteral("Shared (WEP)"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("OWE"))),
             QStringLiteral("Enhanced Open (OWE)"));
}

void TestWifiProfileScanner::security_enterprise() {
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA"))),
             QStringLiteral("WPA-Enterprise"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA2"))),
             QStringLiteral("WPA2-Enterprise"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA3"))),
             QStringLiteral("WPA3-Enterprise"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPA3ENT"))),
             QStringLiteral("WPA3-Enterprise"));
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
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(xml), QString());
    // Differential control on the real skeleton: the SAME document with only <authentication>
    // removed must still refuse, so an "infer the type from <encryption>/<useOneX> when the
    // element is missing" fallback cannot hide behind a document that has no such siblings.
    QString stripped = profileXml(QStringLiteral("WPA2PSK"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(stripped), QStringLiteral("WPA2-Personal"));
    stripped.remove(QStringLiteral("<authentication>WPA2PSK</authentication>"));
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(stripped), QString());
    // Present-but-blank is the sibling refusal path (the regex captures the single space, which
    // trims away to nothing): the caller gets nothing back, never raw whitespace as a type.
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("   "))), QString());
}

void TestWifiProfileScanner::security_emptyXml() {
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(QString()), QString());
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(QStringLiteral("   \r\n")), QString());
    // Bare token text is not a profile: the parser keys off the element, so loose text in the
    // document never becomes a security type.
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(QStringLiteral("WPA2PSK")), QString());
    // A truncated element is malformed, not a security type: the closing </authentication> anchor
    // is required, so a half-written WlanGetProfile buffer fails closed instead of reporting
    // "WPA2-Personal".
    const QString truncated = QStringLiteral("<WLANProfile><authentication>WPA2PSK");
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(truncated), QString());
}

void TestWifiProfileScanner::security_localeIndependent() {
    // The whole point of the WLAN-API rewrite: the token is schema text, so a non-English Windows
    // (which localizes netsh CONSOLE output but never the profile XML) still yields the right type.
    QCOMPARE(sak::wifiSecurityTypeFromProfileXml(profileXml(QStringLiteral("WPAPSK"))),
             QStringLiteral("WPA-Personal"));
}

QTEST_MAIN(TestWifiProfileScanner)
#include "test_wifi_profile_scanner.moc"
