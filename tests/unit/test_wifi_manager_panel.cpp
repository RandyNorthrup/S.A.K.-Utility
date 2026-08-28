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
    void installRefusesIncompleteRows();
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

// What a security label MEANS is no longer decided here. This panel used to carry its own
// resolver, and it disagreed with the shared one on the panel's OWN default label: it tested for
// "WPA3" before "WPA", so "WPA/WPA2/WPA3" -- the first entry in the panel's security combo, and
// therefore the setting every network gets unless the technician changes it -- resolved to a
// WPA3SAE-ONLY profile, which cannot associate with a WPA2-only access point and is rejected
// outright by pre-1903 Windows. Resolution now happens once, in sak::addWifiProfileWindows, whose
// own test pins every mapping including that combined label.
//
// What remains the panel's own rule is whether a ROW is complete enough to install at all.
void WifiManagerPanelTests::installRefusesIncompleteRows() {
    const auto row = [](const QString& ssid, const QString& security) {
        WifiManagerPanel::WifiConfig cfg;
        cfg.ssid = ssid;
        cfg.security = security;
        return cfg;
    };

    // A complete row is NOT refused -- without this the assertions below would pass just as
    // happily against a rule that refused everything.
    QCOMPARE(WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QStringLiteral("Lab"), QStringLiteral("WPA/WPA2/WPA3"))),
             QString());

    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QString(), QStringLiteral("WPA/WPA2/WPA3")))
                 .isEmpty());
    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QStringLiteral("   "), QStringLiteral("WPA/WPA2/WPA3")))
                 .isEmpty());

    // An EMPTY security label is refused HERE, even though the shared installer would read it as
    // "caller did not specify, use the interoperable default". That default is right for the
    // assistant's connect_wifi, where security is an optional argument; it is wrong for this
    // table, where every row has a security column, so an empty one means the row is malformed.
    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(row(QStringLiteral("Lab"), QString()))
                 .isEmpty());
    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QStringLiteral("Lab"), QStringLiteral("  ")))
                 .isEmpty());

    // The two refusals name different causes, so a log line says which field was missing.
    QVERIFY(WifiManagerPanel::wifiProfileInstallRefusal(
                row(QString(), QStringLiteral("WPA/WPA2/WPA3"))) !=
            WifiManagerPanel::wifiProfileInstallRefusal(row(QStringLiteral("Lab"), QString())));
}

QTEST_MAIN(WifiManagerPanelTests)
#include "test_wifi_manager_panel.moc"
