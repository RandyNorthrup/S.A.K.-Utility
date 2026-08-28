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

/// Install a machine-wide WLAN profile for @p ssid WITHOUT connecting, by running
/// `netsh wlan add profile` shell-free (argv vector, no interpolation). Requires administrator
/// rights -- a non-elevated call fails honestly (profile_added stays false). Fails closed for an
/// empty/unsafe SSID (a double quote or control character), an over-length SSID, or an
/// enterprise/802.1X security type, before running anything. connect_issued is always false: this
/// function does not attempt to associate.
///
/// This is the seam the WiFi manager panel installs through. The panel used to carry its own
/// security resolver and its own profile-XML builder, and they disagreed with these: the panel's
/// resolver mapped its OWN default label "WPA/WPA2/WPA3" to a WPA3SAE-ONLY profile, because it
/// tested for "WPA3" before "WPA". Such a profile cannot associate with a WPA2-only access point
/// and is rejected outright by pre-1903 Windows, so the panel's default setting produced profiles
/// that could not connect. resolveWlanAuth treats a combined label as the interoperable WPA2-PSK
/// it is.
[[nodiscard]] WifiConnectResult addWifiProfileWindows(const QString& ssid,
                                                      const QString& password,
                                                      const QString& security,
                                                      bool hidden);

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
