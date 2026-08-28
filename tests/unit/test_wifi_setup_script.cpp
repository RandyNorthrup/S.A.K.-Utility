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
    void refusesOverlongSsid();
    void neutralizesBatchMetacharacters();
    void usesUniqueTempFileAndWipesOnBothPaths();
    void usesPassphraseOnlyForWpa();
    void usesFullyQualifiedToolPaths();
    void buildsWpa3SaeProfile();
    void refusesEnterpriseSecurity();
    void refusesUnrecognizedSecurity();
    void connectRefusesEnterpriseSecurity();
    void connectRefusesEmptySsid();
    void connectRefusesUnsafeSsid();
    void connectRefusesOverlongSsid();
    void addProfileAppliesTheSameRefusalsAsConnect();
    void addProfileNeverIssuesAConnect();
};

// addWifiProfileWindows is the half of connectWifiWindows the WiFi manager panel installs
// through. It must fail closed on exactly the same inputs -- if the two ever diverge, the panel
// regains a way to install a profile the assistant would have refused, which is the duplication
// this seam was extracted to remove. Every case below returns before any process is run, so these
// tests have no system side effects.
void TestWifiSetupScript::addProfileAppliesTheSameRefusalsAsConnect() {
    struct Case {
        QString ssid;
        QString security;
    };
    const QList<Case> refused = {
        {QString(), QStringLiteral("wpa2")},                         // empty SSID
        {QStringLiteral("Net\"work"), QStringLiteral("wpa2")},       // quote in SSID
        {QStringLiteral("Net\nwork"), QStringLiteral("wpa2")},       // control char in SSID
        {QString(33, QLatin1Char('a')), QStringLiteral("wpa2")},     // over the 32-byte WLAN limit
        {QStringLiteral("Lab"), QStringLiteral("WPA2-Enterprise")},  // 802.1X needs EAP config
        {QStringLiteral("Lab"), QStringLiteral("Frobnicate9")},      // unrecognized: no downgrade
    };
    for (const Case& c : refused) {
        const auto added =
            sak::addWifiProfileWindows(c.ssid, QStringLiteral("pw"), c.security, false);
        const auto connected =
            sak::connectWifiWindows(c.ssid, QStringLiteral("pw"), c.security, false);
        QVERIFY(!added.profile_added);
        QVERIFY(!added.error.isEmpty());
        // The SAME refusal, word for word. A test that only checked "both failed" would pass even
        // if the two disagreed about why, which is how the panel's old resolver stayed wrong.
        QCOMPARE(added.error, connected.error);
    }
}

void TestWifiSetupScript::addProfileNeverIssuesAConnect() {
    // Installing a profile must not associate. The panel's "add these networks to Windows" button
    // saves credentials for later; it does not move the technician's machine onto the network it
    // is saving. A refused input proves it without touching the radio.
    const auto result =
        sak::addWifiProfileWindows(QString(), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(!result.connect_issued);
}

// connectWifiWindows must fail closed (no netsh, no temp file) for an empty/unsafe/over-length
// SSID. These paths return before any process is run, so the tests have no system side effects.
void TestWifiSetupScript::connectRefusesEmptySsid() {
    const auto r =
        sak::connectWifiWindows(QString(), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(!r.profile_added);
    QVERIFY(!r.connect_issued);
    QCOMPARE(r.error,
             QStringLiteral("SSID is empty or contains a double quote / control character"));
}

void TestWifiSetupScript::connectRefusesUnsafeSsid() {
    const auto quoted = sak::connectWifiWindows(
        QStringLiteral("Net\"work"), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(!quoted.profile_added);
    QCOMPARE(quoted.error,
             QStringLiteral("SSID is empty or contains a double quote / control character"));
    const auto ctrl = sak::connectWifiWindows(
        QStringLiteral("Net\nwork"), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(!ctrl.profile_added);
    QCOMPARE(ctrl.error,
             QStringLiteral("SSID is empty or contains a double quote / control character"));
}

void TestWifiSetupScript::connectRefusesOverlongSsid() {
    const auto r = sak::connectWifiWindows(
        QString(200, QLatin1Char('a')), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(!r.profile_added);
    QCOMPARE(r.error, QStringLiteral("SSID exceeds the 32-byte WLAN limit"));

    // Same byte rule on the connect path: 11 characters, 33 octets. It returns before any netsh
    // process or temp file, so there are no side effects.
    const auto wide =
        sak::connectWifiWindows(QString::fromUtf8(QByteArray("\xE4\xB8\xAD").repeated(11)),
                                QStringLiteral("pw"),
                                QStringLiteral("wpa2"),
                                false);
    QVERIFY(!wide.profile_added);
    QVERIFY(!wide.connect_issued);
    QCOMPARE(wide.error, QStringLiteral("SSID exceeds the 32-byte WLAN limit"));
}

void TestWifiSetupScript::usesUniqueTempFileAndWipesOnBothPaths() {
    // B9-15: the profile XML holds the plaintext passphrase, so the generated .cmd
    // must NOT write it to a fixed, predictable %TEMP% path, and must delete it on
    // both the add-profile failure and success branches.
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("MyNet"), QStringLiteral("secret123"), QStringLiteral("wpa2"), false);
    QVERIFY(!script.isEmpty());

    // The old predictable filename must be gone.
    QVERIFY(!script.contains(QStringLiteral("wifi_profile_sak.xml")));
    // A kernel-unique temp file is allocated instead.
    QVERIFY(script.contains(QStringLiteral("GetTempFileName()")));
    QVERIFY(script.contains(QStringLiteral("if not defined PROFILE_XML")));
    // The `if not defined` guard is only sound because the script first scopes the environment
    // (`setlocal`) and WIPES any INHERITED PROFILE_XML *before* the allocation. Without that
    // wipe an attacker who seeds PROFILE_XML (e.g. via HKCU\Environment before the user launches
    // the .cmd elevated) satisfies the guard when the for/f allocation produces no output, and
    // the script then performs an ELEVATED Set-Content write plus `del` against the
    // attacker-chosen path. Pin the wipe AND its order: setlocal < wipe < allocation < guard.
    const int scope_at = script.indexOf(QStringLiteral("\r\nsetlocal\r\n"));
    const int wipe_at = script.indexOf(QStringLiteral("set \"PROFILE_XML=\"\r\n"));
    const int alloc_at = script.indexOf(QStringLiteral("GetTempFileName()"));
    const int guard_at = script.indexOf(QStringLiteral("if not defined PROFILE_XML"));
    QVERIFY(scope_at >= 0);
    QVERIFY(wipe_at > scope_at);
    QVERIFY(alloc_at > wipe_at);
    QVERIFY(guard_at > alloc_at);

    // The XML is deleted on BOTH paths: the failure branch (before `exit /b 1`) and
    // the success path (before `netsh wlan connect`). Two `del` calls prove it.
    // Position, not just count: the failure branch must delete the XML BEFORE `exit /b 1` (a
    // non-elevated run must not leave the plaintext passphrase in %TEMP%), and the success path
    // must delete it BEFORE the connect line.
    const QString kDel = QStringLiteral("del \"%PROFILE_XML%\" 2>nul");
    // Search from the add-profile failure branch: `exit /b 1` also appears earlier, in the
    // `if not defined PROFILE_XML` block, so an unanchored search would pin nothing.
    const qsizetype fail_branch = script.indexOf(QStringLiteral("if %errorlevel% neq 0 (\r\n"));
    QVERIFY(fail_branch >= 0);
    const qsizetype fail_del = script.indexOf(kDel, fail_branch);
    const qsizetype fail_exit = script.indexOf(QStringLiteral("exit /b 1"), fail_branch);
    const qsizetype branch_end = script.indexOf(QStringLiteral("\r\n)\r\n"), fail_branch);
    QVERIFY(fail_del >= 0);
    QVERIFY(fail_exit >= 0);
    QVERIFY(branch_end >= 0);
    QVERIFY(fail_del < fail_exit);   // wiped before the failure branch bails out
    QVERIFY(fail_del < branch_end);  // and the wipe is INSIDE that branch, not after it

    // The second `del` is on the success path, ahead of the connect line.
    const qsizetype success_del = script.indexOf(kDel, branch_end);
    const qsizetype connect_pos = script.indexOf(
        QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan connect name=\"MyNet\""));
    QVERIFY(success_del >= 0);
    QVERIFY(connect_pos >= 0);
    QVERIFY(connect_pos > success_del);

    // Exactly those two wipes, no extras.
    QCOMPARE(script.count(kDel), 2);
}

void TestWifiSetupScript::usesPassphraseOnlyForWpa() {
    // The <keyMaterial>-emitting condition: only WPA2 (anything not open/none/wep).
    QVERIFY(sak::wifiSecurityUsesPassphrase(QStringLiteral("wpa2")));
    QVERIFY(sak::wifiSecurityUsesPassphrase(QStringLiteral("")));  // default -> WPA2
    QVERIFY(!sak::wifiSecurityUsesPassphrase(QStringLiteral("open")));
    QVERIFY(!sak::wifiSecurityUsesPassphrase(QStringLiteral("none")));
    QVERIFY(!sak::wifiSecurityUsesPassphrase(QStringLiteral("wep")));

    // wifiSecurityUsesPassphrase returns false BOTH for a resolved "open" auth and for a REFUSED
    // (nullopt) security, so the negatives above cannot tell "builds a keyless profile" from
    // "builds nothing at all". Pin the positive observable for each: "none" must still build an
    // OPEN profile, and "wep" must build an open/WEP profile that KEEPS the key as a
    // <networkKey> -- a keyless WEP profile cannot connect.
    const QString none_script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("CafeGuest"), QString(), QStringLiteral("none"), false);
    QVERIFY(!none_script.isEmpty());
    const QString none_xml = decodedXml(none_script);
    QVERIFY(none_xml.contains(QStringLiteral("<authentication>open</authentication>")));
    QVERIFY(none_xml.contains(QStringLiteral("<encryption>none</encryption>")));
    QVERIFY(!none_xml.contains(QStringLiteral("keyMaterial")));

    const QString wep_script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("OldNet"), QStringLiteral("wepkey123"), QStringLiteral("wep"), false);
    QVERIFY(!wep_script.isEmpty());
    const QString wep_xml = decodedXml(wep_script);
    QVERIFY(wep_xml.contains(QStringLiteral("<authentication>open</authentication>")));
    QVERIFY(wep_xml.contains(QStringLiteral("<encryption>WEP</encryption>")));
    QVERIFY(wep_xml.contains(QStringLiteral("<keyType>networkKey</keyType>")));
    QVERIFY(wep_xml.contains(QStringLiteral("<keyMaterial>wepkey123</keyMaterial>")));
    QVERIFY(!wep_xml.contains(QStringLiteral("passPhrase")));
}

void TestWifiSetupScript::usesFullyQualifiedToolPaths() {
    // The generated elevated .cmd must invoke powershell/netsh via their absolute
    // System32 paths so a planted-exe on %PATH% or in the CWD cannot hijack them.
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("MyNet"), QStringLiteral("secret123"), QStringLiteral("wpa2"), false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(
        QStringLiteral("\"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\"")));
    QVERIFY(script.contains(QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\"")));
    // No bare tool invocation may survive (would be resolved via %PATH%).
    QVERIFY(!script.contains(QStringLiteral("(`powershell ")));
    QVERIFY(!script.contains(QStringLiteral("\r\npowershell ")));
    QVERIFY(!script.contains(QStringLiteral("\r\nnetsh ")));
}

void TestWifiSetupScript::buildsWpa3SaeProfile() {
    // WPA3-Personal must map to WPA3SAE/AES and still emit the passphrase, not be
    // silently downgraded to WPA2PSK.
    const QString script = sak::buildWifiSetupScriptWindows(QStringLiteral("MyNet"),
                                                            QStringLiteral("secret123"),
                                                            QStringLiteral("WPA3-Personal"),
                                                            false);
    QVERIFY(!script.isEmpty());
    const QString xml = decodedXml(script);
    QVERIFY(xml.contains(QStringLiteral("<authentication>WPA3SAE</authentication>")));
    QVERIFY(xml.contains(QStringLiteral("<encryption>AES</encryption>")));
    QVERIFY(xml.contains(QStringLiteral("<keyType>passPhrase</keyType>")));
    QVERIFY(xml.contains(QStringLiteral("<keyMaterial>secret123</keyMaterial>")));
    QVERIFY(sak::wifiSecurityUsesPassphrase(QStringLiteral("WPA3-Personal")));

    // The two NEGATIVE arms of the WPA3-SAE test: a combined / transitional label -- notably the
    // WiFi panel's OWN default "WPA/WPA2/WPA3" -- must resolve to the interoperable WPA2PSK
    // profile, never a WPA3SAE-only one (it cannot associate with a WPA2-only AP and is rejected
    // outright by pre-1903 Windows).
    const QString mixed_xml =
        decodedXml(sak::buildWifiSetupScriptWindows(QStringLiteral("MyNet"),
                                                    QStringLiteral("secret123"),
                                                    QStringLiteral("WPA/WPA2/WPA3"),
                                                    false));
    QVERIFY(mixed_xml.contains(QStringLiteral("<authentication>WPA2PSK</authentication>")));
    QVERIFY(!mixed_xml.contains(QStringLiteral("WPA3SAE")));
}

void TestWifiSetupScript::refusesEnterpriseSecurity() {
    // Enterprise/802.1X needs an EAP/OneX config this builder cannot produce; fail
    // closed rather than emit a downgraded WPA2PSK profile.
    QVERIFY(sak::buildWifiSetupScriptWindows(QStringLiteral("CorpNet"),
                                             QStringLiteral("pw"),
                                             QStringLiteral("WPA2-Enterprise"),
                                             false)
                .isEmpty());
    QVERIFY(sak::buildWifiSetupScriptWindows(QStringLiteral("CorpNet"),
                                             QStringLiteral("pw"),
                                             QStringLiteral("WPA3-Enterprise"),
                                             false)
                .isEmpty());
    // Each substring the enterprise screen tests must be screened on its OWN. A label that ALSO
    // names a WPA-personal scheme -- what a scanner actually reports for a real enterprise AP --
    // cannot fall through to the terminal unrecognized-label refusal, so deleting any single arm
    // turns it into a WPA2PSK profile carrying the user's passphrase instead of a refusal.
    const QStringList enterprise_labels{QStringLiteral("WPA2-802.1X"),
                                        // "802.11X" is NOT a superset of "802.1X" (6th char is
                                        // '1', not 'X'), so it needs its own arm.
                                        QStringLiteral("WPA2-802.11X"),
                                        QStringLiteral("WPA2-EAP"),
                                        QStringLiteral("WPA-PEAP"),
                                        QStringLiteral("WPA2-OneX")};
    for (const QString& label : enterprise_labels) {
        QVERIFY2(sak::buildWifiSetupScriptWindows(
                     QStringLiteral("CorpNet"), QStringLiteral("pw"), label, false)
                     .isEmpty(),
                 qPrintable(label));
        QVERIFY2(!sak::wifiSecurityUsesPassphrase(label), qPrintable(label));
    }
    // A bare "802.1X" label also refuses -- but it names no WPA scheme, so on its own it cannot
    // distinguish the enterprise guard from the unrecognized-label refusal.
    QVERIFY(sak::buildWifiSetupScriptWindows(
                QStringLiteral("CorpNet"), QStringLiteral("pw"), QStringLiteral("802.1X"), false)
                .isEmpty());
    // An unsupported type has no passphrase.
    QVERIFY(!sak::wifiSecurityUsesPassphrase(QStringLiteral("WPA2-Enterprise")));
}

void TestWifiSetupScript::refusesUnrecognizedSecurity() {
    // resolveWlanAuth's SECOND fail-closed exit: a NON-EMPTY label matching no known scheme --
    // e.g. one derived from a rogue AP's beacon -- must be refused, never silently resolved to
    // the interoperable WPA2-PSK default, which would emit a profile carrying the user's
    // passphrase. "Frobnicate9" names no ENTERPRISE/802.1X/EAP/ONEX, WPA/PSK/SAE, WEP/SHARED or
    // NONE/OPEN/OWE token, so it reaches the terminal return.
    const QString unknown = QStringLiteral("Frobnicate9");
    QVERIFY(sak::buildWifiSetupScriptWindows(
                QStringLiteral("MyNet"), QStringLiteral("secret123"), unknown, false)
                .isEmpty());
    QVERIFY(!sak::wifiSecurityUsesPassphrase(unknown));
    // connectWifiWindows refuses in validateConnectInputs, before any temp file or netsh
    // process, so this has no system side effects.
    const auto r = sak::connectWifiWindows(
        QStringLiteral("MyNet"), QStringLiteral("secret123"), unknown, false);
    QVERIFY(!r.profile_added);
    QVERIFY(!r.connect_issued);
    QCOMPARE(r.error,
             QStringLiteral("Enterprise/802.1X WiFi security is not supported by this connector"));
}

void TestWifiSetupScript::connectRefusesEnterpriseSecurity() {
    // connectWifiWindows must fail closed before touching a temp file or netsh.
    const auto r = sak::connectWifiWindows(
        QStringLiteral("CorpNet"), QStringLiteral("pw"), QStringLiteral("WPA2-Enterprise"), false);
    QVERIFY(!r.profile_added);
    QVERIFY(!r.connect_issued);
    QCOMPARE(r.error,
             QStringLiteral("Enterprise/802.1X WiFi security is not supported by this connector"));
}

void TestWifiSetupScript::buildsWpaScriptWithProfileAndConnect() {
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("MyNet"), QStringLiteral("secret123"), QStringLiteral("wpa2"), false);
    QVERIFY(!script.isEmpty());
    QVERIFY(
        script.contains(QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan add profile")));
    QVERIFY(script.contains(
        QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan connect name=\"MyNet\"")));
    QVERIFY(script.contains(QStringLiteral("echo Network: MyNet")));

    const QString xml = decodedXml(script);
    QVERIFY(xml.contains(QStringLiteral("<authentication>WPA2PSK</authentication>")));
    QVERIFY(xml.contains(QStringLiteral("<keyMaterial>secret123</keyMaterial>")));
    QVERIFY(xml.contains(QStringLiteral("<name>MyNet</name>")));
    // <nonBroadcast> in BOTH polarities, so the `hidden` argument is exercised end to end -- it
    // is the only element that argument controls.
    QVERIFY(xml.contains(QStringLiteral("<nonBroadcast>false</nonBroadcast>")));

    // A hidden (non-broadcasting) network MUST mark the profile nonBroadcast, otherwise Windows
    // never actively probes for the SSID and the connect silently never happens.
    const QString hidden_script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("MyNet"), QStringLiteral("secret123"), QStringLiteral("wpa2"), true);
    QVERIFY(!hidden_script.isEmpty());
    const QString hidden_xml = decodedXml(hidden_script);
    QVERIFY(hidden_xml.contains(QStringLiteral("<nonBroadcast>true</nonBroadcast>")));
    // The flag changes nothing else in the profile record.
    QVERIFY(hidden_xml.contains(QStringLiteral("<authentication>WPA2PSK</authentication>")));
    QVERIFY(hidden_xml.contains(QStringLiteral("<keyMaterial>secret123</keyMaterial>")));
}

void TestWifiSetupScript::openNetworkOmitsKeyMaterial() {
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("CafeGuest"), QString(), QStringLiteral("open"), false);
    QVERIFY(!script.isEmpty());
    const QString xml = decodedXml(script);
    QVERIFY(xml.contains(QStringLiteral("<authentication>open</authentication>")));
    QVERIFY(xml.contains(QStringLiteral("<encryption>none</encryption>")));
    QVERIFY(!xml.contains(QStringLiteral("keyMaterial")));

    // The auth-is-open arm of the <sharedKey> guard, exercised ON ITS OWN. With an empty
    // password above, `!password.isEmpty()` alone suppresses the block, so the open-vs-WEP half
    // is never reached. An open/guest network built with a passphrase still in hand -- the WiFi
    // panel keeps the field populated when the user flips security to Open, and the script
    // generator takes password and security as independent arguments -- must emit NO key
    // material: otherwise the plaintext passphrase ships inside a .cmd on disk while the
    // embeds-password note, computed from the security label rather than the XML, still reports
    // that the script holds no password.
    const QString open_with_pw = sak::buildWifiSetupScriptWindows(
        QStringLiteral("CafeGuest"), QStringLiteral("secret123"), QStringLiteral("open"), false);
    QVERIFY(!open_with_pw.isEmpty());
    const QString pw_xml = decodedXml(open_with_pw);
    QVERIFY(pw_xml.contains(QStringLiteral("<authentication>open</authentication>")));
    QVERIFY(pw_xml.contains(QStringLiteral("<encryption>none</encryption>")));
    QVERIFY(!pw_xml.contains(QStringLiteral("sharedKey")));
    QVERIFY(!pw_xml.contains(QStringLiteral("keyMaterial")));
    QVERIFY(!pw_xml.contains(QStringLiteral("secret123")));
    // Nor may it leak in the .cmd outside the base64 payload (echo lines, connect line).
    QVERIFY(!open_with_pw.contains(QStringLiteral("secret123")));
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

void TestWifiSetupScript::refusesOverlongSsid() {
    // B9-18: an 802.11 SSID is at most 32 octets. A 33-byte SSID must be refused, and
    // a 32-byte one accepted. Enforced on UTF-8 byte length, not character count.
    const QString ssid33(33, QLatin1Char('a'));
    QVERIFY(sak::buildWifiSetupScriptWindows(
                ssid33, QStringLiteral("pw"), QStringLiteral("wpa2"), false)
                .isEmpty());

    const QString ssid32(32, QLatin1Char('a'));
    QVERIFY(!sak::buildWifiSetupScriptWindows(
                 ssid32, QStringLiteral("pw"), QStringLiteral("wpa2"), false)
                 .isEmpty());

    // The limit is on UTF-8 OCTETS, not QString characters -- and every case above is ASCII,
    // where the two are identical. U+4E2D encodes as 3 bytes, so 11 of them are only 11
    // characters but 33 octets: a char-count check would accept this SSID and hand netsh a
    // profile the 802.11 32-octet limit forbids.
    const QString ssid_wide33 = QString::fromUtf8(QByteArray("\xE4\xB8\xAD").repeated(11));
    QCOMPARE(ssid_wide33.size(), 11);
    QCOMPARE(ssid_wide33.toUtf8().size(), 33);
    QVERIFY(sak::buildWifiSetupScriptWindows(
                ssid_wide33, QStringLiteral("pw"), QStringLiteral("wpa2"), false)
                .isEmpty());

    // ...and the same SSID one character shorter (10 chars / 30 octets) is still accepted, so
    // the refusal above is the byte limit and not a blanket rejection of non-ASCII.
    const QString ssid_wide30 = QString::fromUtf8(QByteArray("\xE4\xB8\xAD").repeated(10));
    QCOMPARE(ssid_wide30.toUtf8().size(), 30);
    QVERIFY(!sak::buildWifiSetupScriptWindows(
                 ssid_wide30, QStringLiteral("pw"), QStringLiteral("wpa2"), false)
                 .isEmpty());
}

void TestWifiSetupScript::neutralizesBatchMetacharacters() {
    // '&' is caret-escaped and '%' is doubled so the SSID cannot inject a command or expand an
    // environment variable when the .cmd runs.
    const QString script = sak::buildWifiSetupScriptWindows(
        QStringLiteral("A&B%C"), QStringLiteral("pw"), QStringLiteral("wpa2"), false);
    QVERIFY(!script.isEmpty());
    // "^&" and "%%" both occur in the script's own boilerplate, so their mere presence was
    // satisfied by the fixture regardless of the SSID. The connect line must carry the FULLY
    // escaped SSID -- '&' caret-escaped AND '%' doubled -- inside the quoted netsh argument.
    QVERIFY(script.contains(
        QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan connect name=\"A^&B%%C\"")));
    // The banner echo interpolates the same escaped form.
    QVERIFY(script.contains(QStringLiteral("echo Network: A^&B%%C\r\n")));
    // A single, undoubled '%' from the SSID must never reach the connect line -- it would be
    // expanded as an environment variable when the elevated .cmd runs.
    QVERIFY(!script.contains(QStringLiteral("name=\"A^&B%C\"")));
    // The raw, unescaped SSID must never appear in the connect line.
    QVERIFY(!script.contains(QStringLiteral("name=\"A&B%C\"")));
}

QTEST_MAIN(TestWifiSetupScript)
#include "test_wifi_setup_script.moc"
