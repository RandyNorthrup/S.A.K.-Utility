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

}  // namespace

QString wifiSecurityTypeFromProfileXml(const QString& xml) {
    // The first (and only) <authentication> element lives under MSM/security/authEncryption. Match
    // it case-insensitively across newlines; the value is a schema token, never localized text.
    static const QRegularExpression re(
        QStringLiteral("<authentication>\\s*([^<]+?)\\s*</authentication>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = re.match(xml);
    if (!match.hasMatch()) {
        return {};
    }
    return friendlyWifiAuthName(match.captured(1));
}

#ifdef Q_OS_WIN

namespace {

/// Read one profile's XML, deriving its security type (and, when requested, its re-importable XML).
/// WlanGetProfile is called WITHOUT WLAN_PROFILE_GET_PLAINTEXT_KEY, so the key stays
/// DPAPI-protected and no plaintext PSK is ever produced. Sets @p detail_ok false on a read
/// failure.
WifiProfileInfo readOneProfile(
    HANDLE handle, const GUID& guid, LPCWSTR profile_name, bool include_xml, bool& detail_ok) {
    WifiProfileInfo info;
    info.profile_name = QString::fromWCharArray(profile_name);
    info.selected = true;

    LPWSTR xml = nullptr;
    DWORD flags = 0;  // 0 -> keyMaterial remains DPAPI-protected (protected=true), never plaintext
    DWORD granted_access = 0;
    const DWORD rc =
        WlanGetProfile(handle, &guid, profile_name, nullptr, &xml, &flags, &granted_access);
    if (rc == ERROR_SUCCESS && xml != nullptr) {
        const QString xml_str = QString::fromWCharArray(xml);
        info.security_type = wifiSecurityTypeFromProfileXml(xml_str);
        if (include_xml) {
            info.xml_data = xml_str;  // real re-importable WLANProfile XML (DPAPI-protected key)
        }
        detail_ok = true;
    } else {
        detail_ok = false;
        // The detail read failed: this record carries no security type and no re-importable
        // XML. Do NOT leave it flagged selected, so a failed/empty profile is never backed up
        // or exported as if it were a real one (fail closed on the incomplete record).
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
                             bool include_xml,
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
        out.append(readOneProfile(
            handle, guid, profile_list->ProfileInfo[p].strProfileName, include_xml, detail_ok));
        if (!detail_ok) {
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
        logger(QStringLiteral("%1 of %2 profiles could not be fully read")
                   .arg(detail_failures)
                   .arg(profiles.size()));
    }
}

}  // namespace

QVector<WifiProfileInfo> scanAllWifiProfiles(const WifiScanLogger& logger,
                                             bool* scan_ok,
                                             bool include_xml) {
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

    QVector<WifiProfileInfo> profiles;
    int detail_failures = 0;
    bool enumeration_complete = true;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        if (!appendInterfaceProfiles(handle,
                                     interfaces->InterfaceInfo[i].InterfaceGuid,
                                     include_xml,
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
                                             bool include_xml) {
    Q_UNUSED(include_xml);
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
