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
#include "sak/process_runner.h"

#include <QDir>
#include <QTemporaryFile>

#include <optional>

namespace sak {

namespace {

constexpr int kEscapedTextReserveMultiplier = 2;
constexpr int kMaxSsidBytes = 32;           // an 802.11 SSID is at most 32 octets
constexpr int kFirstPrintableAscii = 0x20;  // bytes below space are C0 controls

// Fully-qualified System32 paths for the tools the generated elevated .cmd invokes.
// Emitting absolute paths (rather than bare `powershell`/`netsh`) defeats a planted-exe
// hijack: a malicious powershell.exe/netsh.exe dropped in the script's working directory
// or earlier on %PATH% would otherwise run with the administrator rights of the script.
constexpr const char* kPowershellExe =
    "\"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\"";
constexpr const char* kNetshExe = "\"%SystemRoot%\\System32\\netsh.exe\"";

// True if the SSID fits the 802.11 32-octet limit. Uses UTF-8 byte length, not
// QString character count: a multi-byte SSID can be <=32 chars yet >32 bytes.
bool ssidWithinByteLimit(const QString& ssid) {
    return ssid.toUtf8().size() <= kMaxSsidBytes;
}

struct WlanAuthConfig {
    QString auth_type;
    QString enc_type;
};

// True if the security string names an enterprise / 802.1X (EAP) network. Such a
// profile needs a full <OneX>/<EAPConfig> block (EAP method, server validation, cert
// or credential selection) that this passphrase-only builder cannot produce, so we
// fail closed rather than emit a WPA2PSK profile that silently downgrades the security.
bool securityIsEnterprise(const QString& upper) {
    return upper.contains("ENTERPRISE") || upper.contains("802.1X") || upper.contains("802.11X") ||
           upper.contains("EAP") || upper.contains("ONEX");
}

// Resolve a Windows WLAN authentication/encryption pair, or std::nullopt for an
// unsupported security type (enterprise/802.1X) that must fail closed.
// A WPA3/SAE-ONLY label maps to WPA3SAE. A combined/transitional label -- notably the WiFi
// panel's own default "WPA/WPA2/WPA3", and any label that also names WPA2 -- must NOT become a
// WPA3SAE-only profile (it would fail to associate with a WPA2-only AP and is rejected outright
// by pre-1903 Windows). Those fall through to the interoperable WPA2PSK default.
bool isWpa3SaeOnly(const QString& upper) {
    return (upper.contains("WPA3") || upper.contains("SAE")) && !upper.contains("WPA2") &&
           !upper.contains("WPA/");
}

std::optional<WlanAuthConfig> resolveWlanAuth(const QString& security) {
    const QString upper = security.toUpper();
    if (securityIsEnterprise(upper)) {
        return std::nullopt;
    }
    // WEP only when the label names WEP and no WPA variant: a mixed label like "WPA2, not WEP"
    // is a WPA2 network, not an (insecure) open/WEP one.
    if (upper.contains("WEP") && !upper.contains("WPA")) {
        return WlanAuthConfig{"open", "WEP"};
    }
    if (upper.contains("NONE") || upper.contains("OPEN") || upper.contains("OWE")) {
        return WlanAuthConfig{"open", "none"};
    }
    if (isWpa3SaeOnly(upper)) {
        return WlanAuthConfig{"WPA3SAE", "AES"};
    }
    return WlanAuthConfig{"WPA2PSK", "AES"};
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
    // WEP resolves to auth "open" but still carries a key: emit it as <networkKey> so a WEP key is
    // NOT silently discarded (a keyless WEP profile cannot connect). WPA/WPA2/WPA3 keys stay
    // passPhrase; a genuinely open/none network (no key) emits no <sharedKey>.
    const bool is_wep = auth.enc_type == QLatin1String("WEP");
    if (!password.isEmpty() && (auth.auth_type != QLatin1String("open") || is_wep)) {
        xml += "  <sharedKey>\r\n";
        xml += is_wep ? "    <keyType>networkKey</keyType>\r\n"
                      : "    <keyType>passPhrase</keyType>\r\n";
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
        if (c == '"' || c.unicode() < kFirstPrintableAscii) {
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
    // setlocal scopes the environment and the explicit clear below wipes any INHERITED PROFILE_XML,
    // so an attacker who seeds PROFILE_XML (e.g. via HKCU\\Environment before an elevated launch)
    // cannot satisfy the "if not defined PROFILE_XML" guard when the for/f allocation produces no
    // output (powershell blocked, GetTempFileName throws): without this the script would run an
    // ELEVATED write and delete against the attacker-chosen path.
    script += "setlocal\r\n";
    script += "echo S.A.K. Utility - WiFi Network Setup Script\r\n";
    script += "echo Network: " + safe_ssid + "\r\n";
    script += "echo.\r\n";
    script += "set \"PROFILE_XML=\"\r\n";
    // Allocate a kernel-chosen, unpredictable temp file (GetTempFileName atomically
    // creates a uniquely named 0-byte file), instead of a fixed %TEMP%\wifi_profile_sak.xml
    // that a local attacker could pre-create/symlink and that two runs would collide on.
    script +=
        "for /f \"usebackq delims=\" %%i in "
        "(`" +
        QString::fromLatin1(kPowershellExe) +
        " -NoProfile -Command \"[System.IO.Path]::GetTempFileName()\"`) "
        "do set \"PROFILE_XML=%%i\"\r\n";
    script += "if not defined PROFILE_XML (\r\n";
    script += "    echo Failed to allocate a temporary profile file.\r\n";
    script += "    pause\r\n";
    script += "    exit /b 1\r\n";
    script += ")\r\n";
    script += QString::fromLatin1(kPowershellExe) +
              " -NoProfile -Command \"[System.Text.Encoding]::UTF8."
              "GetString([System.Convert]::FromBase64String('" +
              xml_base64 +
              "')) | Set-Content -LiteralPath $env:PROFILE_XML"
              " -Encoding UTF8\"\r\n";
    script += QString::fromLatin1(kNetshExe) +
              " wlan add profile filename=\"%PROFILE_XML%\""
              " user=all\r\n";
    // Wipe the plaintext-password XML immediately after netsh consumes it, on BOTH the
    // failure and success paths, so the credentials do not linger in %TEMP%.
    script += "if %errorlevel% neq 0 (\r\n";
    script +=
        "    echo Failed to add WiFi profile."
        " Run as Administrator.\r\n";
    script += "    del \"%PROFILE_XML%\" 2>nul\r\n";
    script += "    pause\r\n";
    script += "    exit /b 1\r\n";
    script += ")\r\n";
    script += "del \"%PROFILE_XML%\" 2>nul\r\n";
    script += QString::fromLatin1(kNetshExe) + " wlan connect name=" + quoted_ssid + "\r\n";
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
    if (!ssidWithinByteLimit(ssid)) {
        sak::logWarning("Refusing to build WiFi script: SSID exceeds the 32-byte WLAN limit");
        return {};
    }
    const std::optional<WlanAuthConfig> auth = resolveWlanAuth(security);
    if (!auth) {
        sak::logWarning(
            "Refusing to build WiFi script: enterprise/802.1X security is not supported");
        return {};
    }
    const QString xml = buildWlanXmlContent(ssid, password, *auth, hidden);
    const QString xml_base64 = QString::fromLatin1(xml.toUtf8().toBase64());
    return buildBatchScript(ssid, xml_base64);
}

bool wifiSecurityUsesPassphrase(const QString& security) {
    // Mirrors buildWlanXmlContent's keyMaterial guard: it emits <keyMaterial> only when the
    // resolved auth is NOT "open" (i.e. WPA2-PSK / WPA3-SAE). Uses the SAME resolveWlanAuth so the
    // two never drift. An unsupported (enterprise/802.1X) type has no passphrase and fails closed.
    const std::optional<WlanAuthConfig> auth = resolveWlanAuth(security);
    return auth && auth->auth_type != QLatin1String("open");
}

// Fail closed on empty / unsafe (quote or control char) / over-length SSID and on
// an unsupported enterprise/802.1X security type BEFORE touching a temp file or
// running netsh. Returns the resolved auth, or nullopt with @p error set.
static std::optional<WlanAuthConfig> validateConnectInputs(const QString& ssid,
                                                           const QString& security,
                                                           QString& error) {
    if (ssid.isEmpty() || !ssidIsBatchSafe(ssid)) {
        error = QStringLiteral("SSID is empty or contains a double quote / control character");
        return std::nullopt;
    }
    if (!ssidWithinByteLimit(ssid)) {
        error = QStringLiteral("SSID exceeds the 32-byte WLAN limit");
        return std::nullopt;
    }
    const std::optional<WlanAuthConfig> auth = resolveWlanAuth(security);
    if (!auth) {
        error =
            QStringLiteral("Enterprise/802.1X WiFi security is not supported by this connector");
    }
    return auth;
}

WifiConnectResult connectWifiWindows(const QString& ssid,
                                     const QString& password,
                                     const QString& security,
                                     bool hidden) {
    WifiConnectResult result;
    const std::optional<WlanAuthConfig> auth = validateConnectInputs(ssid, security, result.error);
    if (!auth) {
        return result;
    }
    const QString xml = buildWlanXmlContent(ssid, password, *auth, hidden);

    // Write the profile XML to a private, auto-removed temp file for `netsh wlan add profile`.
    QTemporaryFile xml_file(QDir::tempPath() + QStringLiteral("/sak_wifi_XXXXXX.xml"));
    xml_file.setAutoRemove(true);
    if (!xml_file.open()) {
        result.error = QStringLiteral("Could not create a temporary WLAN profile file");
        return result;
    }
    xml_file.write(xml.toUtf8());
    xml_file.flush();

    // netsh runs shell-free via an argv vector (no shell, no interpolation). Both calls need
    // administrator rights, so a non-elevated run fails HONESTLY here rather than silently.
    // The interpreter is the System32-qualified netsh, never the bare name: CreateProcess
    // searches the current directory ahead of System32, and these calls are elevated.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        result.error = QStringLiteral("Cannot resolve the System32 netsh.exe path");
        return result;
    }
    constexpr int kNetshTimeoutMs = 15'000;
    const auto netsh_error = [](const ProcessResult& r) {
        const QString err = r.std_err.trimmed();
        return err.isEmpty() ? r.std_out.trimmed() : err;
    };
    const ProcessResult add = runProcess(netsh_exe,
                                         {QStringLiteral("wlan"),
                                          QStringLiteral("add"),
                                          QStringLiteral("profile"),
                                          QStringLiteral("filename=") + xml_file.fileName(),
                                          QStringLiteral("user=all")},
                                         kNetshTimeoutMs);
    if (!add.succeeded()) {
        result.error =
            add.timed_out
                ? QStringLiteral("netsh timed out adding the WLAN profile")
                : QStringLiteral(
                      "Failed to add the WLAN profile (this needs administrator rights): %1")
                      .arg(netsh_error(add));
        return result;
    }
    result.profile_added = true;

    const ProcessResult conn = runProcess(
        netsh_exe,
        {QStringLiteral("wlan"), QStringLiteral("connect"), QStringLiteral("name=") + ssid},
        kNetshTimeoutMs);
    result.connect_issued = conn.succeeded();
    if (!result.connect_issued) {
        result.error = conn.timed_out
                           ? QStringLiteral("netsh timed out issuing connect")
                           : QStringLiteral("profile added, but connect could not be issued: %1")
                                 .arg(netsh_error(conn));
    }
    return result;
}

}  // namespace sak
