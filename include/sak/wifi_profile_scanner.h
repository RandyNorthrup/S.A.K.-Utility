// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_profile_scanner.h
/// @brief Utility for scanning Windows WiFi profiles via the native WLAN API

#pragma once

#include "sak/user_profile_types.h"

#include <QString>
#include <QVector>

#include <functional>

namespace sak {

/// @brief Callback for reporting scan progress messages
using WifiScanLogger = std::function<void(const QString&)>;

/// @brief Map a WLANProfile <authentication> element to a friendly security-type label.
/// @param xml A WLANProfile XML document (as returned by WlanGetProfile).
/// @return e.g. "WPA2-Personal", "WPA3-Personal", "Open"; empty when no <authentication> is
/// present.
/// @note The <authentication> value is a schema enum (open/WPAPSK/WPA2PSK/WPA3SAE/...), NOT
///       localized console text, so this is language-neutral on every Windows UI language -- unlike
///       the old parse that keyed off the English "Authentication :" line of `netsh show profile`.
[[nodiscard]] QString wifiSecurityTypeFromProfileXml(const QString& xml);

/// @brief Whether a WLANProfile describes a hidden (non-broadcasting) network.
/// @param xml A WLANProfile XML document.
/// @return true only for an explicit `<nonBroadcast>true</nonBroadcast>`; false when the element
///         says false, is absent, is blank, or holds anything else.
/// @note Like the security type, this reads a schema BOOLEAN rather than console text. The parse
///       it replaces keyed off the English "Network broadcast : Don't broadcast" line of
///       `netsh wlan show profile`, which reports nothing recognisable on a non-English Windows.
[[nodiscard]] bool wifiHiddenFromProfileXml(const QString& xml);

/// @brief Extract the cleartext pre-shared key from a WLANProfile document.
/// @param xml A WLANProfile XML document.
/// @return The `<keyMaterial>` text, but ONLY when the enclosing `<sharedKey>` also carries
///         `<protected>false</protected>`. Returns empty in every other case.
/// @warning The `protected` check is the whole point of this function and is not a formality.
///          When WlanGetProfile is called WITHOUT WLAN_PROFILE_GET_PLAINTEXT_KEY, `keyMaterial`
///          still contains a value -- it is the DPAPI CIPHERTEXT, a long hex blob. Returning that
///          as "the password" would put an unusable string in front of a technician and, worse,
///          into whatever they exported next. So the element alone is never sufficient evidence
///          that a key is readable: `protected` must say so.
/// @note Enterprise (802.1X) profiles have no `<sharedKey>` at all and correctly yield empty.
[[nodiscard]] QString wifiPlaintextKeyFromProfileXml(const QString& xml);

/// @brief Whether a scan should ask Windows to decrypt each profile's pre-shared key.
enum class WifiKeyMaterial {
    /// WlanGetProfile runs with no flags: the key stays DPAPI-protected and no plaintext PSK is
    /// ever materialized in this process. Correct for backup (the protected XML re-imports fine)
    /// and for the headless list op (which surfaces name + security type only).
    Protected,

    /// WlanGetProfile runs with WLAN_PROFILE_GET_PLAINTEXT_KEY, so `plaintext_key` is filled in
    /// for profiles whose key Windows agrees to decrypt for this token. Ask for this ONLY when a
    /// human is about to be shown or handed the key -- it is what the WiFi panel's "add known
    /// networks" does, because re-deploying a network elsewhere needs the actual passphrase.
    Plaintext
};

/// @brief Scan all Windows WiFi profiles using the native WLAN API (wlanapi).
/// @param logger Optional callback for progress/error messages
/// @param scan_ok Optional out-param: set true only when the scan is COMPLETE and authoritative --
///        the top-level enumeration ran (WLAN service available, WlanOpenHandle/WlanEnumInterfaces
///        succeeded) AND every discovered profile's detail read succeeded. Set false when the
///        enumeration itself failed OR any per-profile WlanGetProfile failed (that profile is
///        returned without re-importable XML, so a backup trusting scan_ok would be silently
///        incomplete). Distinguishes a genuine "0 saved profiles" from a failed/partial scan --
///        without it an empty or short return is a fail-open honesty hole.
/// @param include_xml When true (default, GUI backup path) each profile's re-importable
///        DPAPI-protected WLANProfile XML is stored in info.xml_data. When false it is parsed for
///        the security type and then discarded: no re-importable key material is ever retained --
///        used by the headless list op, which surfaces only profile name + security type. Either
///        way WlanGetProfile is called WITHOUT WLAN_PROFILE_GET_PLAINTEXT_KEY, so the key stays
///        DPAPI-protected and no plaintext PSK is ever materialized (and nothing is written to
///        disk, unlike the old `netsh wlan export` temp-dir path).
/// @param key_material Whether to ask Windows to decrypt each profile's PSK. Defaults to
///        WifiKeyMaterial::Protected, so a caller that does not think about key material never
///        accidentally produces one.
/// @return Vector of discovered WiFi profile info. Every record carries `hidden`; `plaintext_key`
///         is populated only under WifiKeyMaterial::Plaintext.
/// @warning Requesting `include_xml` together with WifiKeyMaterial::Plaintext is REFUSED: the
///          returned XML would then hold the cleartext key in `xml_data`, which IS serialized by
///          WifiProfileInfo::toJson and would therefore write the PSK into any backup manifest
///          built from the result. The call fails closed (empty vector, `scan_ok` false) rather
///          than quietly dropping one of the two requests, because either silent choice would be
///          wrong for the caller who asked for both.
[[nodiscard]] QVector<WifiProfileInfo> scanAllWifiProfiles(
    const WifiScanLogger& logger = nullptr,
    bool* scan_ok = nullptr,
    bool include_xml = true,
    WifiKeyMaterial key_material = WifiKeyMaterial::Protected);

}  // namespace sak
