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

    // -- wifiHiddenFromProfileXml ----------------------------------------
    void hidden_explicitTrue();
    void hidden_explicitFalseAndAbsent();
    void hidden_onlyExactTrueCounts();

    // -- wifiPlaintextKeyFromProfileXml ----------------------------------
    void key_returnedWhenProtectedIsFalse();
    void key_refusedWhenProtectedIsTrue();
    void key_refusedWhenProtectedIsAbsent();
    void key_absentSharedKeyYieldsNothing();
    void key_truncatedElementYieldsNothing();

    // -- the scan's fail-closed contract ---------------------------------
    void scan_refusesPlaintextKeyTogetherWithXmlRetention();
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

/// A WLANProfile with an <MSM><security><sharedKey> block, as WlanGetProfile returns for a
/// PSK network. @p protected_value is the literal text of <protected>, and @p key_material the
/// literal text of <keyMaterial> -- the pair the plaintext-key reader has to judge.
QString sharedKeyXml(const QString& protected_value, const QString& key_material) {
    return QStringLiteral(
               "<?xml version=\"1.0\"?>\r\n"
               "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\r\n"
               "  <name>MyNet</name>\r\n"
               "  <SSIDConfig>\r\n"
               "    <SSID><name>MyNet</name></SSID>\r\n"
               "    <nonBroadcast>false</nonBroadcast>\r\n"
               "  </SSIDConfig>\r\n"
               "  <MSM>\r\n"
               "    <security>\r\n"
               "      <authEncryption>\r\n"
               "        <authentication>WPA2PSK</authentication>\r\n"
               "        <encryption>AES</encryption>\r\n"
               "      </authEncryption>\r\n"
               "      <sharedKey>\r\n"
               "        <keyType>passPhrase</keyType>\r\n"
               "        <protected>%1</protected>\r\n"
               "        <keyMaterial>%2</keyMaterial>\r\n"
               "      </sharedKey>\r\n"
               "    </security>\r\n"
               "  </MSM>\r\n"
               "</WLANProfile>\r\n")
        .arg(protected_value, key_material);
}

/// The DPAPI ciphertext Windows puts in <keyMaterial> when the plaintext flag was NOT granted.
/// Shaped like the real thing -- a long uppercase hex blob -- because the whole risk is that it
/// looks enough like key material to be handed to someone as a password.
const QString kProtectedBlob = QStringLiteral(
    "01000000D08C9DDF0115D1118C7A00C04FC297EB01000000B8A1F0C7E4D2A34B9F0E5C6D7A8B9C0D");

/// A WLANProfile whose SSIDConfig carries the given <nonBroadcast> text.
QString nonBroadcastXml(const QString& value) {
    return QStringLiteral(
               "<WLANProfile><SSIDConfig><SSID><name>MyNet</name></SSID>"
               "<nonBroadcast>%1</nonBroadcast></SSIDConfig></WLANProfile>")
        .arg(value);
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

// ---------------------------------------------------------------------------
// wifiHiddenFromProfileXml
// ---------------------------------------------------------------------------

void TestWifiProfileScanner::hidden_explicitTrue() {
    QVERIFY(sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("true"))));
    // Schema booleans are written both ways by different exporters; casing must not decide
    // whether a network is treated as hidden.
    QVERIFY(sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("TRUE"))));
    QVERIFY(sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("True"))));
    // Surrounding whitespace is formatting, not meaning.
    QVERIFY(sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("\r\n  true  \r\n"))));
}

void TestWifiProfileScanner::hidden_explicitFalseAndAbsent() {
    QVERIFY(!sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("false"))));
    // Absent element: the schema default is broadcasting, and the full realistic document (which
    // does carry an SSIDConfig) must not acquire a hidden flag from somewhere else.
    QVERIFY(!sak::wifiHiddenFromProfileXml(
        QStringLiteral("<WLANProfile><SSIDConfig><SSID><name>MyNet</name></SSID>"
                       "</SSIDConfig></WLANProfile>")));
    QVERIFY(!sak::wifiHiddenFromProfileXml(QString()));
    QVERIFY(!sak::wifiHiddenFromProfileXml(profileXml(QStringLiteral("WPA2PSK"))));
}

void TestWifiProfileScanner::hidden_onlyExactTrueCounts() {
    // Anything that is not the boolean "true" leaves the network visible. A truthiness test that
    // accepted a non-empty value -- or the digit 1 -- would mark ordinary networks hidden, and a
    // hidden profile that is not actually hidden re-deploys as a network that never connects.
    QVERIFY(!sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("1"))));
    QVERIFY(!sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("yes"))));
    QVERIFY(!sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("truthy"))));
    QVERIFY(!sak::wifiHiddenFromProfileXml(nonBroadcastXml(QString())));
    QVERIFY(!sak::wifiHiddenFromProfileXml(nonBroadcastXml(QStringLiteral("   "))));
    // A truncated element is malformed, not a hidden network.
    QVERIFY(!sak::wifiHiddenFromProfileXml(QStringLiteral("<SSIDConfig><nonBroadcast>true")));
}

// ---------------------------------------------------------------------------
// wifiPlaintextKeyFromProfileXml
// ---------------------------------------------------------------------------

void TestWifiProfileScanner::key_returnedWhenProtectedIsFalse() {
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 sharedKeyXml(QStringLiteral("false"), QStringLiteral("hunter2-correct-horse"))),
             QStringLiteral("hunter2-correct-horse"));
    // Casing of the boolean must not gate a key the caller is entitled to read.
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 sharedKeyXml(QStringLiteral("FALSE"), QStringLiteral("hunter2-correct-horse"))),
             QStringLiteral("hunter2-correct-horse"));
    // A passphrase may legitimately contain characters that look like markup boundaries in a
    // careless parse; only '<' actually terminates the element.
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 sharedKeyXml(QStringLiteral("false"), QStringLiteral("p@ss w0rd!&amp;-/=+"))),
             QStringLiteral("p@ss w0rd!&amp;-/=+"));
}

void TestWifiProfileScanner::key_refusedWhenProtectedIsTrue() {
    // THE security assertion of this file. protected=true means <keyMaterial> holds DPAPI
    // CIPHERTEXT, which a reader that trusted the element alone would return as "the password".
    // The blob is present and non-empty, so only the protected check can produce this refusal.
    const QString xml = sharedKeyXml(QStringLiteral("true"), kProtectedBlob);
    QVERIFY(xml.contains(kProtectedBlob));  // the fixture really does carry a key-shaped value
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(xml), QString());
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 sharedKeyXml(QStringLiteral("TRUE"), kProtectedBlob)),
             QString());
    // Differential control: the SAME document with protected flipped to false DOES yield the
    // value, so the refusal above is the protected check and not an unparseable fixture.
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 sharedKeyXml(QStringLiteral("false"), kProtectedBlob)),
             kProtectedBlob);
}

void TestWifiProfileScanner::key_refusedWhenProtectedIsAbsent() {
    // Fail closed on silence: a profile with no <protected> element has not told us the key is
    // readable, so it is not. Absence must never be treated as permission.
    QString xml = sharedKeyXml(QStringLiteral("false"), QStringLiteral("hunter2-correct-horse"));
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(xml), QStringLiteral("hunter2-correct-horse"));
    xml.remove(QStringLiteral("<protected>false</protected>"));
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(xml), QString());
    // Blank and unrecognized values are silence too, not consent.
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 sharedKeyXml(QStringLiteral("   "), QStringLiteral("hunter2"))),
             QString());
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 sharedKeyXml(QStringLiteral("maybe"), QStringLiteral("hunter2"))),
             QString());
}

void TestWifiProfileScanner::key_absentSharedKeyYieldsNothing() {
    // An 802.1X enterprise profile has no <sharedKey> at all, and an open network has no key.
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(profileXml(QStringLiteral("WPA2"))), QString());
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(profileXml(QStringLiteral("open"))), QString());
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(QString()), QString());
    // protected=false with an empty keyMaterial is a readable profile that has no key: empty, and
    // notably NOT the literal text of some neighbouring element.
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(sharedKeyXml(QStringLiteral("false"), QString())),
             QString());
}

void TestWifiProfileScanner::key_truncatedElementYieldsNothing() {
    // A half-written WlanGetProfile buffer must fail closed rather than return whatever bytes
    // followed the opening tag.
    QCOMPARE(sak::wifiPlaintextKeyFromProfileXml(
                 QStringLiteral("<sharedKey><protected>false</protected><keyMaterial>hunter2")),
             QString());
}

// ---------------------------------------------------------------------------
// scanAllWifiProfiles fail-closed contract
// ---------------------------------------------------------------------------

void TestWifiProfileScanner::scan_refusesPlaintextKeyTogetherWithXmlRetention() {
    // Asking for plaintext keys AND retained XML would put every PSK in the clear inside
    // xml_data, which WifiProfileInfo::toJson serializes -- so a backup manifest built from the
    // result would carry them. The scan refuses the combination outright.
    //
    // This runs before any WLAN call, so it is deterministic on a machine with no wireless
    // adapter and in CI: the refusal is a contract check, not a hardware-dependent outcome.
    bool scan_ok = true;  // seeded WRONG on purpose: the refusal must clear it, not leave it
    const QVector<sak::WifiProfileInfo> refused = sak::scanAllWifiProfiles(
        nullptr, &scan_ok, /*include_xml=*/true, sak::WifiKeyMaterial::Plaintext);
    QVERIFY(refused.isEmpty());
    QVERIFY(!scan_ok);

    // The refusal is specific to the COMBINATION. Requesting protected XML is the ordinary backup
    // path and must not be caught by it. That call may legitimately return nothing on a machine
    // with no WLAN service, so assert only that it is not rejected for the same reason -- i.e.
    // that the guard did not widen into "no XML for anybody".
    bool xml_only_ok = false;
    const QVector<sak::WifiProfileInfo> xml_only = sak::scanAllWifiProfiles(
        nullptr, &xml_only_ok, /*include_xml=*/true, sak::WifiKeyMaterial::Protected);
    if (xml_only_ok) {
        // A successful scan proves the guard let it through; every record must then be free of
        // plaintext, because Protected was requested.
        for (const sak::WifiProfileInfo& info : xml_only) {
            QVERIFY(info.plaintext_key.isEmpty());
        }
    }
}

QTEST_MAIN(TestWifiProfileScanner)
#include "test_wifi_profile_scanner.moc"
