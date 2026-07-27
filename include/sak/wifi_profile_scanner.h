// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_profile_scanner.h
/// @brief Utility for scanning Windows WiFi profiles via netsh

#pragma once

#include "sak/user_profile_types.h"

#include <QString>
#include <QVector>

#include <functional>

namespace sak {

/// @brief Callback for reporting scan progress messages
using WifiScanLogger = std::function<void(const QString&)>;

/// @brief Parse WiFi profile names from netsh "show profiles" output
/// @param output Raw stdout from "netsh wlan show profiles"
/// @return List of profile name strings
[[nodiscard]] QStringList parseWifiProfileNames(const QString& output);

/// @brief Extract authentication/security type from netsh profile detail
/// @param detail_output Raw stdout from "netsh wlan show profile name=X"
/// @return Security type string (e.g. "WPA2-Personal") or empty
[[nodiscard]] QString parseWifiSecurityType(const QString& detail_output);

/// @brief Scan all Windows WiFi profiles using netsh wlan commands
/// @param logger Optional callback for progress/error messages
/// @param scan_ok Optional out-param: set false when the profile enumeration itself failed (netsh
///        errored / WLAN service unavailable), true otherwise. Distinguishes a genuine "0 saved
///        profiles" from a failed scan -- without it an empty return is a fail-open honesty hole
///        (a failed enumeration looks identical to a machine with no saved networks).
/// @param include_xml When true (default, GUI backup path) each profile is exported to
///        DPAPI-protected WLANProfile XML (info.xml_data). When false the export is skipped: no
///        temp-dir/DPAPI churn and no re-importable key material is ever materialized -- used by
///        the headless list op, which surfaces only profile name + security type to the model.
/// @return Vector of discovered WiFi profile info
[[nodiscard]] QVector<WifiProfileInfo> scanAllWifiProfiles(const WifiScanLogger& logger = nullptr,
                                                           bool* scan_ok = nullptr,
                                                           bool include_xml = true);

}  // namespace sak
