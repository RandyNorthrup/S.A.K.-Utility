// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_setup_script.h
/// @brief Build an injection-safe Windows WLAN setup (.cmd) script for a network.

#pragma once

#include <QString>

namespace sak {

/// Build a Windows .cmd that adds a WLAN profile (via a base64-encoded netsh XML written to a temp
/// file) and connects to @p ssid. Injection-hardened: an SSID that is empty, or that cannot be
/// safely embedded in the quoted `netsh wlan connect name="..."` argument (contains a double quote
/// or a control character), yields an EMPTY string rather than an injectable script -- callers
/// treat empty as a refusal. Caret metacharacters and '%' in the SSID are neutralized for the .cmd.
///
/// @param security  "WEP", "NONE"/"OPEN", or anything else (treated as WPA2-PSK / AES).
/// @param hidden    marks the profile nonBroadcast (a non-broadcasting network).
[[nodiscard]] QString buildWifiSetupScriptWindows(const QString& ssid,
                                                  const QString& password,
                                                  const QString& security,
                                                  bool hidden);

/// True if @p security resolves to a passphrase-bearing network (WPA2-PSK) whose profile actually
/// embeds the supplied password as <keyMaterial>. False for open/none AND for WEP (both resolve to
/// a non-passphrase auth, so buildWifiSetupScriptWindows emits no keyMaterial even if a password is
/// given). Lets a caller report an honest "does the script contain the password" flag.
[[nodiscard]] bool wifiSecurityUsesPassphrase(const QString& security);

/// Outcome of a live connectWifiWindows() attempt. profile_added is the DURABLE change (the WLAN
/// profile is installed machine-wide); connect_issued means netsh accepted the connect request (the
/// association itself may still be pending / out of range). error is empty on full success.
struct WifiConnectResult {
    bool profile_added = false;
    bool connect_issued = false;
    QString error;
};

/// Install a WLAN profile for @p ssid and connect to it NOW, by running `netsh wlan add profile` +
/// `netsh wlan connect` shell-free (argv vector, no interpolation). Requires administrator rights
/// -- a non-elevated call fails honestly (profile_added stays false). Fails closed for an
/// empty/unsafe SSID (a double quote or control character) or an over-length SSID before running
/// anything.
[[nodiscard]] WifiConnectResult connectWifiWindows(const QString& ssid,
                                                   const QString& password,
                                                   const QString& security,
                                                   bool hidden);

}  // namespace sak
