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
/// @return Vector of discovered WiFi profile info
[[nodiscard]] QVector<WifiProfileInfo> scanAllWifiProfiles(const WifiScanLogger& logger = nullptr,
                                                           bool* scan_ok = nullptr,
                                                           bool include_xml = true);

}  // namespace sak
