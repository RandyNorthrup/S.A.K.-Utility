// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_wifi_manager_panel.cpp
/// @brief Unit tests for WifiManagerPanel::buildWindowsScript SSID injection safety

#include "sak/wifi_manager_panel.h"

#include <QtTest/QtTest>

using namespace sak;

class WifiManagerPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void normalSsidProducesScript();
    void percentSsidIsDoubled();
    void ampersandSsidIsCaretEscaped();
    void quoteSsidIsRejected();
    void controlCharSsidIsRejected();
    void envVarSsidNotExpandedInConnect();
    void jsonWriteFailsClosedOnShortWrite();
    void securityMappingRefusesUnsupportedAndPreservesWpa3();
};

// A benign SSID yields a runnable script with the base64 profile and netsh call.
void WifiManagerPanelTests::normalSsidProducesScript() {
    const QString script = WifiManagerPanel::buildWindowsScript("MyNetwork", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    // netsh is invoked by its absolute System32 path (anti-hijack), so match the
    // command + args rather than a bare "netsh" token.
    QVERIFY(
        script.contains(QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan add profile")));
    QVERIFY(script.contains(
        QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan connect name=\"MyNetwork\"")));
}

// P-W10 (SSID injection): '%' must be doubled so it is not expanded as an
// environment variable when the generated .cmd runs.
void WifiManagerPanelTests::percentSsidIsDoubled() {
    const QString script = WifiManagerPanel::buildWindowsScript("Net%PATH%", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("Net%%PATH%%")));
    QVERIFY(!script.contains(QStringLiteral("Net%PATH%")));  // raw form must never appear
}

// Caret metacharacters stay neutralized with a leading '^'.
void WifiManagerPanelTests::ampersandSsidIsCaretEscaped() {
    const QString script = WifiManagerPanel::buildWindowsScript("A&B|C", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("A^&B^|C")));
}

// A double quote would break out of the quoted netsh argument: fail closed.
void WifiManagerPanelTests::quoteSsidIsRejected() {
    const QString script =
        WifiManagerPanel::buildWindowsScript("Evil\" & calc & \"", "pw", "WPA2", false);
    QVERIFY(script.isEmpty());
}

// Control characters break the .cmd line structure: fail closed.
void WifiManagerPanelTests::controlCharSsidIsRejected() {
    QVERIFY(WifiManagerPanel::buildWindowsScript("Net\nrogue", "pw", "WPA2", false).isEmpty());
    QVERIFY(WifiManagerPanel::buildWindowsScript(QStringLiteral("Net\rX"), "pw", "WPA2", false)
                .isEmpty());
}

// The connect line must not carry an expandable env-var reference either.
void WifiManagerPanelTests::envVarSsidNotExpandedInConnect() {
    const QString script = WifiManagerPanel::buildWindowsScript("%COMSPEC%", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("name=\"%%COMSPEC%%\"")));
    QVERIFY(!script.contains(QStringLiteral("name=\"%COMSPEC%\"")));
}

// Fail-closed rule: the credential-table JSON write reports success only when
// every byte landed AND the atomic commit succeeded. A short write or a failed
// commit must never be reported as success (a truncated credential table).
void WifiManagerPanelTests::jsonWriteFailsClosedOnShortWrite() {
    // Full write + successful commit -> success.
    QVERIFY(WifiManagerPanel::jsonWriteSucceeded(128, 128, true));
    // Short write (partial byte count) -> failure regardless of commit flag.
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(64, 128, true));
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(64, 128, false));
    // Full byte count but the commit failed -> failure.
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(128, 128, false));
    // QIODevice::write returns -1 on error -> failure.
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(-1, 128, true));
}

// Fail-closed rule: WLAN profile security must never be silently downgraded. WPA3-Personal maps
// to WPA3SAE (not WPA2-PSK), the supported personal/open/WEP types map to concrete auth+enc, and
// Enterprise / unknown / malformed / empty values are REFUSED (empty authType) rather than coerced
// to WPA2-PSK.
void WifiManagerPanelTests::securityMappingRefusesUnsupportedAndPreservesWpa3() {
    const auto wpa2 = WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("WPA2-Personal"));
    QCOMPARE(wpa2.first, QStringLiteral("WPA2PSK"));
    QCOMPARE(wpa2.second, QStringLiteral("AES"));

    const auto wpa = WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("WPA-Personal"));
    QCOMPARE(wpa.first, QStringLiteral("WPA2PSK"));

    // WPA3-Personal must NOT be downgraded to WPA2-PSK.
    const auto wpa3 = WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("WPA3-Personal"));
    QCOMPARE(wpa3.first, QStringLiteral("WPA3SAE"));
    QCOMPARE(wpa3.second, QStringLiteral("AES"));

    const auto open = WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("Open"));
    QCOMPARE(open.first, QStringLiteral("open"));
    QCOMPARE(open.second, QStringLiteral("none"));

    const auto wep = WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("WEP"));
    QCOMPARE(wep.first, QStringLiteral("open"));
    QCOMPARE(wep.second, QStringLiteral("WEP"));

    // Refused (fail closed): empty authType means the caller must not install a profile.
    QVERIFY(WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("WPA2-Enterprise"))
                .first.isEmpty());
    QVERIFY(WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("WPA3-Enterprise"))
                .first.isEmpty());
    QVERIFY(WifiManagerPanel::wlanAuthEncForSecurity(QStringLiteral("not-a-mode")).first.isEmpty());
    QVERIFY(WifiManagerPanel::wlanAuthEncForSecurity(QString()).first.isEmpty());
}

QTEST_MAIN(WifiManagerPanelTests)
#include "test_wifi_manager_panel.moc"
