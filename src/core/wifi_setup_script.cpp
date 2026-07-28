// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_setup_script.cpp
/// @brief Injection-safe Windows WLAN setup (.cmd) script builder.
///
/// Extracted from the WiFi Manager GUI panel so the same battle-tested, injection-hardened builder
/// backs both the panel's "export setup script" action and the headless
/// network.generate_wifi_setup_script tool.

#include "sak/wifi_setup_script.h"

#include "sak/logger.h"

namespace sak {

namespace {

constexpr int kEscapedTextReserveMultiplier = 2;

struct WlanAuthConfig {
    QString auth_type;
    QString enc_type;
};

WlanAuthConfig resolveWlanAuth(const QString& security) {
    const QString upper = security.toUpper();
    if (upper.contains("WEP")) {
        return {"open", "WEP"};
    }
    if (upper.contains("NONE") || upper.contains("OPEN")) {
        return {"open", "none"};
    }
    return {"WPA2PSK", "AES"};
}

QString buildWlanXmlContent(const QString& ssid,
                            const QString& password,
                            const WlanAuthConfig& auth,
                            bool hidden) {
    const QString escaped_ssid = ssid.toHtmlEscaped();
    const QString hidden_str = hidden ? "true" : "false";

    QString xml;
    xml += "<?xml version=\"1.0\"?>\r\n";
    xml +=
        "<WLANProfile xmlns=\"http://www.microsoft.com/networking/"
        "WLAN/profile/v1\">\r\n";
    xml += "  <name>" + escaped_ssid + "</name>\r\n";
    xml += "  <SSIDConfig>\r\n";
    xml += "    <SSID><name>" + escaped_ssid + "</name></SSID>\r\n";
    xml += "    <nonBroadcast>" + hidden_str + "</nonBroadcast>\r\n";
    xml += "  </SSIDConfig>\r\n";
    xml += "  <connectionType>ESS</connectionType>\r\n";
    xml += "  <connectionMode>auto</connectionMode>\r\n";
    xml += "  <MSM><security><authEncryption>\r\n";
    xml += "    <authentication>" + auth.auth_type + "</authentication>\r\n";
    xml += "    <encryption>" + auth.enc_type + "</encryption>\r\n";
    xml += "    <useOneX>false</useOneX>\r\n";
    xml += "  </authEncryption>\r\n";
    if (!password.isEmpty() && auth.auth_type != "open") {
        xml += "  <sharedKey>\r\n";
        xml += "    <keyType>passPhrase</keyType>\r\n";
        xml += "    <protected>false</protected>\r\n";
        xml += "    <keyMaterial>" + password.toHtmlEscaped() + "</keyMaterial>\r\n";
        xml += "  </sharedKey>\r\n";
    }
    xml += "  </security></MSM>\r\n";
    xml += "</WLANProfile>\r\n";
    return xml;
}

// Escape for safe use in a batch (.cmd) file -- prevent command injection.
// The caret metacharacters (& | > < ^ ! ( )) are neutralized with a leading ^,
// and '%' is doubled to '%%' so an SSID like "%COMSPEC%" is not expanded as an
// environment variable when the generated script runs. Callers must have already
// rejected '"' and control characters (see ssidIsBatchSafe); those cannot be
// represented safely inside the quoted `netsh wlan connect name="..."` argument
// and reach here only through a caller bug.
QString escapeBatchString(const QString& text) {
    static const QString kCaretMetacharacters = QStringLiteral("&|><^!()");
    QString result;
    result.reserve(text.size() * kEscapedTextReserveMultiplier);
    for (const QChar c : text) {
        if (c == '%') {
            result += QLatin1String("%%");  // literal percent in a .cmd file
            continue;
        }
        if (kCaretMetacharacters.contains(c)) {
            result += '^';
        }
        result += c;
    }
    return result;
}

// A double quote breaks out of the quoted netsh argument, and control characters
// (CR/LF/NUL and other C0 codes) break the .cmd line structure -- neither can be
// safely embedded, so a script must not be generated for such an SSID. A crafted
// SSID broadcast by a rogue access point can reach here via the scan-and-add flow.
bool ssidIsBatchSafe(const QString& ssid) {
    for (const QChar c : ssid) {
        if (c == '"' || c.unicode() < 0x20) {
            return false;
        }
    }
    return true;
}

QString buildBatchScript(const QString& ssid, const QString& xml_base64) {
    const QString safe_ssid = escapeBatchString(ssid);
    const QString quoted_ssid = "\"" + safe_ssid + "\"";

    QString script;
    script += "@echo off\r\n";
    script += "echo S.A.K. Utility - WiFi Network Setup Script\r\n";
    script += "echo Network: " + safe_ssid + "\r\n";
    script += "echo.\r\n";
    script += "set PROFILE_XML=%TEMP%\\wifi_profile_sak.xml\r\n";
    script +=
        "powershell -Command \"[System.Text.Encoding]::UTF8."
        "GetString([System.Convert]::FromBase64String('" +
        xml_base64 +
        "')) | Set-Content -Path '%PROFILE_XML%'"
        " -Encoding UTF8\"\r\n";
    script +=
        "netsh wlan add profile filename=\"%PROFILE_XML%\""
        " user=all\r\n";
    script += "if %errorlevel% neq 0 (\r\n";
    script +=
        "    echo Failed to add WiFi profile."
        " Run as Administrator.\r\n";
    script += "    del \"%PROFILE_XML%\" 2>nul\r\n";
    script += "    pause\r\n";
    script += "    exit /b 1\r\n";
    script += ")\r\n";
    script += "del \"%PROFILE_XML%\" 2>nul\r\n";
    script += "netsh wlan connect name=" + quoted_ssid + "\r\n";
    script += "if %errorlevel% neq 0 (\r\n";
    script +=
        "    echo Network profile added but could not connect"
        " immediately.\r\n";
    script +=
        "    echo The network will connect automatically"
        " when in range.\r\n";
    script += ") else (\r\n";
    script += "    echo Successfully connected to " + safe_ssid + "!\r\n";
    script += ")\r\n";
    script += "pause\r\n";
    return script;
}

}  // namespace

QString buildWifiSetupScriptWindows(const QString& ssid,
                                    const QString& password,
                                    const QString& security,
                                    bool hidden) {
    // Fail closed: an empty or unsafe SSID yields no script rather than an injectable one.
    if (ssid.isEmpty() || !ssidIsBatchSafe(ssid)) {
        sak::logWarning(
            "Refusing to build WiFi script: SSID is empty or contains unsafe characters");
        return {};
    }
    const WlanAuthConfig auth = resolveWlanAuth(security);
    const QString xml = buildWlanXmlContent(ssid, password, auth, hidden);
    const QString xml_base64 = QString::fromLatin1(xml.toUtf8().toBase64());
    return buildBatchScript(ssid, xml_base64);
}

bool wifiSecurityUsesPassphrase(const QString& security) {
    // Mirrors buildWlanXmlContent's keyMaterial guard: it emits <keyMaterial> only when the
    // resolved auth is NOT "open" (i.e. WPA2-PSK). Uses the SAME resolveWlanAuth so the two never
    // drift.
    return resolveWlanAuth(security).auth_type != QLatin1String("open");
}

}  // namespace sak
