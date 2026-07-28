// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_wifi_setup_script.cpp
/// @brief Unit tests for sak::buildWifiSetupScriptWindows (injection-safe WLAN .cmd builder).

#include "sak/wifi_setup_script.h"

#include <QtTest/QtTest>

class TestWifiSetupScript : public QObject {
    Q_OBJECT

    // Extract and decode the base64 WLAN-profile XML embedded in the generated .cmd.
    static QString decodedXml(const QString& script) {
        const QString marker = QStringLiteral("FromBase64String('");
        const int start = script.indexOf(marker);
        if (start < 0) {
            return {};
        }
        const int from = start + marker.size();
        const int end = script.indexOf(QLatin1Char('\''), from);
        if (end < 0) {
            return {};
        }
        const QString b64 = script.mid(from, end - from);
        return QString::fromUtf8(QByteArray::fromBase64(b64.toLatin1()));
    }

private Q_SLOTS:
    void buildsWpaScriptWithProfileAndConnect();
    void openNetworkOmitsKeyMaterial();
    void refusesUnsafeSsidWithQuote();
    void refusesSsidWithControlChar();
    void refusesEmptySsid();
    void neutralizesBatchMetacharacters();
    void usesPassphraseOnlyForWpa();
};

void TestWifiSetupScript::usesPassphraseOnlyForWpa() {
    // The <keyMaterial>-emitting condition: only WPA2 (anything not open/none/wep).
    QVERIFY(sak::wifiSecurityUsesPassphrase(QStringLiteral("wpa2")));
    QVERIFY(sak::wifiSecurityUsesPassphrase(QStringLiteral("")));  // default -> WPA2
    QVERIFY(!sak::wifiSecurityUsesPassphrase(QStringLiteral("open")));
    QVERIFY(!sak::wifiSecurityUsesPassphrase(QStringLiteral("none")));
    QVERIFY(!sak::wifiSecurityUsesPassphrase(QStringLiteral("wep")));
}

void TestWifiSetupScript::buildsWpaScriptWithProfileAndConnect() {
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("MyNet"), QStringLiteral("secret123"), QStringLiteral("wpa2"), false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("netsh wlan add profile")));
    QVERIFY(script.contains(QStringLiteral("netsh wlan connect name=\"MyNet\"")));
    QVERIFY(script.contains(QStringLiteral("echo Network: MyNet")));

    const QString xml = decodedXml(script);
    QVERIFY(xml.contains(QStringLiteral("<authentication>WPA2PSK</authentication>")));
    QVERIFY(xml.contains(QStringLiteral("<keyMaterial>secret123</keyMaterial>")));
    QVERIFY(xml.contains(QStringLiteral("<name>MyNet</name>")));
}

void TestWifiSetupScript::openNetworkOmitsKeyMaterial() {
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("CafeGuest"), QString(), QStringLiteral("open"), false);
    QVERIFY(!script.isEmpty());
    const QString xml = decodedXml(script);
    QVERIFY(xml.contains(QStringLiteral("<authentication>open</authentication>")));
    QVERIFY(!xml.contains(QStringLiteral("keyMaterial")));
}

void TestWifiSetupScript::refusesUnsafeSsidWithQuote() {
    // A double quote would break out of the quoted `netsh ... name="..."` argument.
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("Net\"work"), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(script.isEmpty());
}

void TestWifiSetupScript::refusesSsidWithControlChar() {
    // A newline (C0 control) would break the .cmd line structure.
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("Net\nwork"), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(script.isEmpty());
}

void TestWifiSetupScript::refusesEmptySsid() {
    QVERIFY(sak::buildWifiSetupScriptWindows(
                QString(), QStringLiteral("pw"), QStringLiteral("wpa2"), false)
                .isEmpty());
}

void TestWifiSetupScript::neutralizesBatchMetacharacters() {
    // '&' is caret-escaped and '%' is doubled so the SSID cannot inject a command or expand an
    // environment variable when the .cmd runs.
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("A&B%C"), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("^&")));
    QVERIFY(script.contains(QStringLiteral("%%")));
    // The raw, unescaped SSID must never appear in the connect line.
    QVERIFY(!script.contains(QStringLiteral("name=\"A&B%C\"")));
}

QTEST_MAIN(TestWifiSetupScript)
#include "test_wifi_setup_script.moc"
