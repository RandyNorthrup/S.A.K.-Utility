// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_analyzer.h
/// @brief WiFi network scanning via Windows Native WiFi API

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief Per-BSSID security determination derived from the 802.11 beacon.
///
/// The Native WiFi "available network" list is aggregated per SSID, so its
/// auth/cipher fields describe an SSID as a whole -- not any single access
/// point. Deriving security from each BSS entry's own capability bit and
/// information elements keeps an evil-twin AP (e.g. an Open AP broadcasting a
/// secured network's SSID) from inheriting a sibling BSSID's "secure" label.
struct WiFiBssSecurity {
    QString authentication;  ///< "Open","WEP","WPA","WPA2","WPA3"
    QString encryption;      ///< "None","WEP","TKIP","AES-CCMP"
    bool isSecure = false;
};

/// @brief WiFi network analysis via wlanapi.dll
///
/// Discovers WiFi networks, measures signal strength, analyzes
/// channel utilization, identifies security types, and performs
/// MAC vendor lookup via OUI database.
class WiFiAnalyzer : public QObject {
    Q_OBJECT

public:
    explicit WiFiAnalyzer(QObject* parent = nullptr);
    ~WiFiAnalyzer() override;

    WiFiAnalyzer(const WiFiAnalyzer&) = delete;
    WiFiAnalyzer& operator=(const WiFiAnalyzer&) = delete;
    WiFiAnalyzer(WiFiAnalyzer&&) = delete;
    WiFiAnalyzer& operator=(WiFiAnalyzer&&) = delete;

    /// @brief Trigger a WiFi scan (blocking)
    void scan();

    /// @brief Start continuous scanning with interval
    void startContinuousScan(int intervalMs = netdiag::kDefaultWifiScanIntervalMs);

    /// @brief Stop continuous scanning
    void stopContinuousScan();

    /// @brief Get last scan results
    [[nodiscard]] QVector<WiFiNetworkInfo> getLastScanResults() const;

    /// @brief Calculate channel utilization from scan results
    [[nodiscard]] static QVector<WiFiChannelUtilization> calculateChannelUtilization(
        const QVector<WiFiNetworkInfo>& networks);

    /// @brief Get currently connected WiFi network info
    [[nodiscard]] WiFiNetworkInfo getCurrentConnection() const;

    /// @brief Check if WiFi adapter is available
    [[nodiscard]] bool isWiFiAvailable() const;

    /// @brief Convert frequency in kHz to channel number
    [[nodiscard]] static int frequencyToChannel(uint32_t freqKHz);

    /// @brief Convert frequency in kHz to band string
    [[nodiscard]] static QString frequencyToBand(uint32_t freqKHz);

    /// @brief Look up vendor by BSSID MAC prefix
    [[nodiscard]] static QString lookupVendor(const QString& bssid);

    /// @brief Derive per-BSSID security from the 802.11 capability "Privacy" bit
    ///        and raw information-element bytes (RSN IE / WPA vendor IE). Pure and
    ///        self-contained so it can be unit-tested with crafted IE buffers.
    /// @param privacyBit  true if the beacon capability Privacy bit is set
    /// @param ie          pointer to the IE byte blob (may be null)
    /// @param ieLen       length of the IE blob in bytes
    [[nodiscard]] static WiFiBssSecurity deriveBssSecurity(bool privacyBit,
                                                           const unsigned char* ie,
                                                           int ieLen);

Q_SIGNALS:
    void scanComplete(QVector<sak::WiFiNetworkInfo> networks);
    void channelUtilizationUpdated(QVector<sak::WiFiChannelUtilization> channels);
    void errorOccurred(QString error);

private:
    QVector<WiFiNetworkInfo> m_lastScan;
    void* m_wlanHandle = nullptr;   ///< HANDLE (opaque to avoid Windows header)
    QTimer* m_scanTimer = nullptr;  ///< Owned; QObject parent = this
    std::atomic<bool> m_wlanInitialized{false};

    bool initializeWlan();
    void cleanupWlan();
    /// Run one scan pass. @p scanError is set true when the scan FAILED at the driver (no WLAN
    /// interface, or every interface's list read errored -- e.g. the radio is off) as opposed to
    /// a genuine empty result (reads succeeded with zero networks). Lets scan() report a failed
    /// scan honestly via errorOccurred rather than a misleading "0 networks" success.
    [[nodiscard]] QVector<WiFiNetworkInfo> performWlanScan(bool triggerScan, bool& scanError);

    /// @brief Format MAC address bytes to colon-separated string
    [[nodiscard]] static QString formatMacAddress(const unsigned char* addr, int length);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::WiFiAnalyzer>,
              "WiFiAnalyzer must not be copyable.");
