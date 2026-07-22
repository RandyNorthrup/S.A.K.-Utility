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
};

// A benign SSID yields a runnable script with the base64 profile and netsh call.
void WifiManagerPanelTests::normalSsidProducesScript() {
    const QString script = WifiManagerPanel::buildWindowsScript("MyNetwork", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("netsh wlan add profile")));
    QVERIFY(script.contains(QStringLiteral("netsh wlan connect name=\"MyNetwork\"")));
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

QTEST_MAIN(WifiManagerPanelTests)
#include "test_wifi_manager_panel.moc"
