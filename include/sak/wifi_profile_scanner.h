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
/// @return Vector of discovered WiFi profile info (with XML data)
[[nodiscard]] QVector<WifiProfileInfo> scanAllWifiProfiles(const WifiScanLogger& logger = nullptr);

}  // namespace sak
