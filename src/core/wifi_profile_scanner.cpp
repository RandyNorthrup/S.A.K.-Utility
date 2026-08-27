// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_profile_scanner.cpp
/// @brief Utility for scanning Windows WiFi profiles via the native WLAN API

#include "sak/wifi_profile_scanner.h"

#include "sak/logger.h"

#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
// wlanapi.h must follow windows.h.
#include <wlanapi.h>
#endif

namespace sak {

namespace {

/// @brief Map a WLANProfile <authentication> schema token to a friendly, language-neutral label.
/// Tokens are the fixed schema enum (open/shared/WPA/WPAPSK/WPA2/WPA2PSK/WPA3*/OWE...), identical
/// on every Windows UI language. Unknown tokens pass through verbatim so a future scheme is not
/// lost.
QString friendlyWifiAuthName(const QString& token) {
    const QString key = token.trimmed().toLower();
    if (key.isEmpty()) {
        return {};
    }
    // Fixed schema tokens (language-neutral) -> friendly labels. An unknown token falls through and
    // is preserved verbatim, so a future auth scheme is surfaced rather than silently dropped.
    static const QList<QPair<QString, QString>> kTable = {
        {QStringLiteral("open"), QStringLiteral("Open")},
        {QStringLiteral("shared"), QStringLiteral("Shared (WEP)")},
        {QStringLiteral("wpapsk"), QStringLiteral("WPA-Personal")},
        {QStringLiteral("wpa"), QStringLiteral("WPA-Enterprise")},
        {QStringLiteral("wpa2psk"), QStringLiteral("WPA2-Personal")},
        {QStringLiteral("wpa2"), QStringLiteral("WPA2-Enterprise")},
        {QStringLiteral("wpa3sae"), QStringLiteral("WPA3-Personal")},
        {QStringLiteral("wpa3ent192"), QStringLiteral("WPA3-Enterprise (192-bit)")},
        {QStringLiteral("wpa3"), QStringLiteral("WPA3-Enterprise")},
        {QStringLiteral("wpa3ent"), QStringLiteral("WPA3-Enterprise")},
        {QStringLiteral("owe"), QStringLiteral("Enhanced Open (OWE)")},
    };
    for (const auto& entry : kTable) {
        if (key == entry.first) {
            return entry.second;
        }
    }
    return token.trimmed();
}

/// Capture the trimmed text of the first @p element in @p xml, or empty when the element is
/// absent, unterminated or blank. Shared by the three element readers below so they cannot drift
/// apart on casing, newline handling or the closing-tag anchor.
QString firstElementText(const QString& xml, const QString& element) {
    // The closing tag is REQUIRED: a truncated WlanGetProfile buffer must fail closed rather than
    // hand back whatever bytes followed the opening tag.
    const QRegularExpression re(QStringLiteral("<%1>\\s*([^<]*?)\\s*</%1>").arg(element),
                                QRegularExpression::CaseInsensitiveOption |
                                    QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = re.match(xml);
    if (!match.hasMatch()) {
        return {};
    }
    return match.captured(1).trimmed();
}

}  // namespace

bool wifiHiddenFromProfileXml(const QString& xml) {
    // Only an explicit "true" hides the network. Absent, blank, "false" or anything unrecognized
    // means broadcasting, which is both the schema default and the safe answer: mislabelling a
    // visible network as hidden makes a re-deployed profile that never connects.
    return firstElementText(xml, QStringLiteral("nonBroadcast"))
               .compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

QString wifiPlaintextKeyFromProfileXml(const QString& xml) {
    // <protected> gates everything. Without WLAN_PROFILE_GET_PLAINTEXT_KEY, Windows still emits a
    // <keyMaterial> element -- holding the DPAPI ciphertext, a long hex blob that looks enough
    // like a key to be mistaken for one. Reading keyMaterial alone would therefore hand a
    // technician an unusable string and let it be exported as a password.
    if (firstElementText(xml, QStringLiteral("protected"))
            .compare(QStringLiteral("false"), Qt::CaseInsensitive) != 0) {
        return {};
    }
    return firstElementText(xml, QStringLiteral("keyMaterial"));
}

QString wifiSecurityTypeFromProfileXml(const QString& xml) {
    // The first (and only) <authentication> element lives under MSM/security/authEncryption. The
    // value is a schema token, never localized text. Read through the same element reader as
    // nonBroadcast and keyMaterial so all three share one notion of "the element is present,
    // closed, and non-blank".
    return friendlyWifiAuthName(firstElementText(xml, QStringLiteral("authentication")));
}

#ifdef Q_OS_WIN

namespace {

/// What a caller asked this scan to produce. Kept as one value so the two functions that thread it
/// down to WlanGetProfile carry a single decision rather than a pair of loose booleans that could
/// be swapped at a call site.
struct ProfileReadRequest {
    bool include_xml{true};
    WifiKeyMaterial key_material{WifiKeyMaterial::Protected};
};

/// Read one profile's XML, deriving its security type, hidden flag, and -- only under
/// WifiKeyMaterial::Plaintext -- its cleartext PSK. Under WifiKeyMaterial::Protected the
/// WLAN_PROFILE_GET_PLAINTEXT_KEY flag is not set, so the key stays DPAPI-protected and no
/// plaintext PSK is ever produced. Sets @p detail_ok false on a read failure.
WifiProfileInfo readOneProfile(HANDLE handle,
                               const GUID& guid,
                               LPCWSTR profile_name,
                               const ProfileReadRequest& request,
                               bool& detail_ok) {
    WifiProfileInfo info;
    info.profile_name = QString::fromWCharArray(profile_name);
    info.selected = true;

    LPWSTR xml = nullptr;
    // In: which key material to return. Out: what WlanGetProfile actually granted. Passing 0 keeps
    // keyMaterial DPAPI-protected (protected=true); the plaintext flag asks Windows to decrypt it,
    // which it does only for a caller holding sufficient privilege. Either way the XML's own
    // <protected> element is what wifiPlaintextKeyFromProfileXml trusts, so a request that Windows
    // silently declined yields no key rather than a hex blob posing as one.
    DWORD flags = (request.key_material == WifiKeyMaterial::Plaintext)
                      ? static_cast<DWORD>(WLAN_PROFILE_GET_PLAINTEXT_KEY)
                      : 0u;
    DWORD granted_access = 0;
    const DWORD rc =
        WlanGetProfile(handle, &guid, profile_name, nullptr, &xml, &flags, &granted_access);
    if (rc == ERROR_SUCCESS && xml != nullptr) {
        const QString xml_str = QString::fromWCharArray(xml);
        info.security_type = wifiSecurityTypeFromProfileXml(xml_str);
        info.hidden = wifiHiddenFromProfileXml(xml_str);
        if (request.key_material == WifiKeyMaterial::Plaintext) {
            info.plaintext_key = wifiPlaintextKeyFromProfileXml(xml_str);
        }
        if (request.include_xml) {
            info.xml_data = xml_str;  // real re-importable WLANProfile XML (DPAPI-protected key)
        }
        detail_ok = true;
    } else {
        detail_ok = false;
        // The detail read failed: this record carries no security type and no re-importable XML.
        // The caller omits it from the results (detail_ok=false); leaving selected=false as well
        // keeps the record itself fail-closed if it is ever inspected directly.
        info.selected = false;
        sak::logWarning("WlanGetProfile failed for '{}'", info.profile_name.toStdString());
    }
    if (xml != nullptr) {
        WlanFreeMemory(xml);
    }
    return info;
}

/// Append every profile on one WLAN interface. Returns false when the interface's profile list
/// could not be read (its profiles are omitted, so the overall enumeration is incomplete); each
/// profile that fails its detail read increments @p detail_failures.
bool appendInterfaceProfiles(HANDLE handle,
                             const GUID& guid,
                             const ProfileReadRequest& request,
                             QVector<WifiProfileInfo>& out,
                             int& detail_failures) {
    WLAN_PROFILE_INFO_LIST* profile_list = nullptr;
    if (WlanGetProfileList(handle, &guid, nullptr, &profile_list) != ERROR_SUCCESS ||
        profile_list == nullptr) {
        // The profile list for this interface could not be read, so its profiles are omitted
        // entirely. Report the incomplete enumeration (the caller folds this into scan_ok)
        // instead of silently returning a partial result as a complete scan.
        sak::logWarning("WlanGetProfileList failed for a WLAN interface -- its profiles omitted");
        return false;
    }
    for (DWORD p = 0; p < profile_list->dwNumberOfItems; ++p) {
        bool detail_ok = false;
        const WifiProfileInfo info = readOneProfile(
            handle, guid, profile_list->ProfileInfo[p].strProfileName, request, detail_ok);
        if (detail_ok) {
            out.append(info);
        } else {
            // Omit the hollow record (no security type, no re-importable XML) rather than
            // appending an incomplete profile; count it so scan_ok fails closed and the caller
            // learns the enumeration was not fully authoritative.
            ++detail_failures;
        }
    }
    WlanFreeMemory(profile_list);
    return true;
}

/// Settle the final scan outcome once every interface has been walked: report completeness
/// through @p scan_ok and tell the logger how many profiles could not be fully read.
void reportScanOutcome(const WifiScanLogger& logger,
                       bool* scan_ok,
                       bool enumeration_complete,
                       int detail_failures,
                       const QVector<WifiProfileInfo>& profiles) {
    if (scan_ok != nullptr) {
        // Fail closed: scan_ok means "complete + authoritative", not merely "the top-level
        // enumeration ran". A per-profile WlanGetProfile failure yields a profile with no
        // re-importable XML, so a backup that trusted scan_ok would silently be incomplete.
        // Report incompleteness through the same status channel the caller already checks.
        *scan_ok = enumeration_complete && (detail_failures == 0);
    }
    if (detail_failures > 0 && logger) {
        // `profiles` now holds only the fully-read records, so the attempted total is those plus
        // the omitted detail failures.
        logger(QStringLiteral("%1 of %2 profiles could not be fully read")
                   .arg(detail_failures)
                   .arg(profiles.size() + detail_failures));
    }
}

}  // namespace

QVector<WifiProfileInfo> scanAllWifiProfiles(const WifiScanLogger& logger,
                                             bool* scan_ok,
                                             bool include_xml,
                                             WifiKeyMaterial key_material) {
    auto fail = [&](const char* why) -> QVector<WifiProfileInfo> {
        sak::logWarning("WLAN enumeration failed: {}", why);
        if (logger) {
            logger(QStringLiteral("WLAN enumeration failed"));
        }
        if (scan_ok != nullptr) {
            *scan_ok = false;  // enumeration FAILED, not "0 profiles" -- honesty for the caller
        }
        return {};
    };

    // Refuse the one combination that would leak: plaintext key material retained inside xml_data,
    // which WifiProfileInfo::toJson DOES serialize, so a backup manifest built from this result
    // would carry every PSK in the clear. Refusing beats honouring one request and dropping the
    // other -- a caller that asked for both has a bug, and either silent choice hides it.
    if (include_xml && key_material == WifiKeyMaterial::Plaintext) {
        return fail("plaintext key material was requested together with XML retention");
    }

    HANDLE handle = nullptr;
    DWORD negotiated_version = 0;
    if (WlanOpenHandle(WLAN_API_VERSION_2_0, nullptr, &negotiated_version, &handle) !=
        ERROR_SUCCESS) {
        return fail("WlanOpenHandle");
    }

    WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
    if (WlanEnumInterfaces(handle, nullptr, &interfaces) != ERROR_SUCCESS ||
        interfaces == nullptr) {
        WlanCloseHandle(handle, nullptr);
        return fail("WlanEnumInterfaces");
    }

    const ProfileReadRequest request{include_xml, key_material};
    QVector<WifiProfileInfo> profiles;
    int detail_failures = 0;
    bool enumeration_complete = true;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        if (!appendInterfaceProfiles(handle,
                                     interfaces->InterfaceInfo[i].InterfaceGuid,
                                     request,
                                     profiles,
                                     detail_failures)) {
            enumeration_complete = false;
        }
    }

    WlanFreeMemory(interfaces);
    WlanCloseHandle(handle, nullptr);

    reportScanOutcome(logger, scan_ok, enumeration_complete, detail_failures, profiles);
    return profiles;
}

#else   // !Q_OS_WIN

QVector<WifiProfileInfo> scanAllWifiProfiles(const WifiScanLogger& logger,
                                             bool* scan_ok,
                                             bool include_xml,
                                             WifiKeyMaterial key_material) {
    Q_UNUSED(include_xml);
    Q_UNUSED(key_material);
    if (logger) {
        logger(QStringLiteral("WiFi scanning is Windows-only"));
    }
    if (scan_ok != nullptr) {
        *scan_ok = false;  // no WLAN API off Windows -> a failed read, never a dishonest empty
    }
    return {};
}

#endif  // Q_OS_WIN

}  // namespace sak
