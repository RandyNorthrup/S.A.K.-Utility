// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_analyzer.cpp
/// @brief WiFi network scanning via Windows Native WiFi API

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "sak/wifi_analyzer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <memory>

#include <winsock2.h>

#include <windows.h>

#include <wlanapi.h>

#pragma comment(lib, "wlanapi.lib")

namespace sak {

namespace {
constexpr int kWlanClientVersion = 2;
constexpr int kMacAddrLen = 6;
constexpr uint32_t kFreq2_4GHzBase = 2'412'000;
constexpr uint32_t kFreq2_4GHzStep = 5000;
constexpr uint32_t kFreq2_4GHzCh14 = 2'484'000;
constexpr uint32_t kFreq5GHzBase = 5'000'000;
constexpr int kChannel2_4GHzBase = 1;
constexpr int kChannel14 = 14;
constexpr int kSignalQualityToDbmBase = -100;
constexpr double kSignalQualityFactor = 0.5;
constexpr int kMaxOuiPrefixLen = 8;

void wlanFreeMemory(void* p) {
    if (p != nullptr) {
        WlanFreeMemory(p);
    }
}

template <typename T>
using WlanPtr = std::unique_ptr<T, void (*)(void*)>;

[[nodiscard]] QString ssidFromDot11Ssid(const DOT11_SSID& ssid) {
    return QString::fromUtf8(reinterpret_cast<const char*>(ssid.ucSSID),
                             static_cast<int>(ssid.uSSIDLength));
}

[[nodiscard]] int signalQualityFromRssi(int rssiDbm) {
    if (rssiDbm >= -50) {
        return 100;
    }
    if (rssiDbm <= -100) {
        return 0;
    }
    return 2 * (rssiDbm + 100);
}

[[nodiscard]] QString bssTypeString(DOT11_BSS_TYPE type) {
    switch (type) {
    case dot11_BSS_type_infrastructure:
        return QStringLiteral("Infrastructure");
    case dot11_BSS_type_independent:
        return QStringLiteral("Ad-hoc");
    default:
        return QStringLiteral("Unknown");
    }
}

[[nodiscard]] QString formatMacAddressString(const unsigned char* addr, int length) {
    if (addr == nullptr || length <= 0) {
        return {};
    }

    QString mac;
    for (int i = 0; i < length; ++i) {
        if (i > 0) {
            mac += QLatin1Char(':');
        }
        mac += QStringLiteral("%1").arg(addr[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return mac;
}

[[nodiscard]] WiFiNetworkInfo networkFromBssEntry(const WLAN_BSS_ENTRY& bss) {
    WiFiNetworkInfo info;

    info.ssid = ssidFromDot11Ssid(bss.dot11Ssid);
    info.bssid = formatMacAddressString(bss.dot11Bssid, kMacAddrLen);

    info.rssiDbm = static_cast<int>(bss.lRssi);
    info.signalQuality = signalQualityFromRssi(info.rssiDbm);

    info.channelFrequencyKHz = bss.ulChCenterFrequency;
    info.channelNumber = WiFiAnalyzer::frequencyToChannel(info.channelFrequencyKHz);
    info.band = WiFiAnalyzer::frequencyToBand(info.channelFrequencyKHz);
    info.bssType = bssTypeString(bss.dot11BssType);

    info.apVendor = WiFiAnalyzer::lookupVendor(info.bssid);
    return info;
}

void appendBssNetworks(const WLAN_BSS_LIST& bssList, QVector<WiFiNetworkInfo>& networks) {
    for (DWORD j = 0; j < bssList.dwNumberOfItems; ++j) {
        const auto& bss = bssList.wlanBssEntries[j];
        networks.append(networkFromBssEntry(bss));
    }
}

void applyAuthAndEncryption(const WLAN_AVAILABLE_NETWORK& net, WiFiNetworkInfo& info) {
    switch (net.dot11DefaultAuthAlgorithm) {
    case DOT11_AUTH_ALGO_80211_OPEN:
        info.authentication = QStringLiteral("Open");
        break;
    case DOT11_AUTH_ALGO_80211_SHARED_KEY:
        info.authentication = QStringLiteral("Shared Key");
        break;
    case DOT11_AUTH_ALGO_WPA:
        info.authentication = QStringLiteral("WPA-Enterprise");
        break;
    case DOT11_AUTH_ALGO_WPA_PSK:
        info.authentication = QStringLiteral("WPA-Personal");
        break;
    case DOT11_AUTH_ALGO_RSNA:
        info.authentication = QStringLiteral("WPA2-Enterprise");
        break;
    case DOT11_AUTH_ALGO_RSNA_PSK:
        info.authentication = QStringLiteral("WPA2-Personal");
        break;
    default:
        if (static_cast<int>(net.dot11DefaultAuthAlgorithm) >= 9) {
            info.authentication = QStringLiteral("WPA3");
        } else {
            info.authentication = QStringLiteral("Unknown");
        }
        break;
    }

    switch (net.dot11DefaultCipherAlgorithm) {
    case DOT11_CIPHER_ALGO_NONE:
        info.encryption = QStringLiteral("None");
        info.isSecure = false;
        break;
    case DOT11_CIPHER_ALGO_WEP40:
    case DOT11_CIPHER_ALGO_WEP104:
    case DOT11_CIPHER_ALGO_WEP:
        info.encryption = QStringLiteral("WEP");
        info.isSecure = true;
        break;
    case DOT11_CIPHER_ALGO_TKIP:
        info.encryption = QStringLiteral("TKIP");
        info.isSecure = true;
        break;
    case DOT11_CIPHER_ALGO_CCMP:
        info.encryption = QStringLiteral("AES-CCMP");
        info.isSecure = true;
        break;
    default:
        info.encryption = QStringLiteral("Other");
        info.isSecure = true;
        break;
    }
}

void applyAvailableNetwork(const WLAN_AVAILABLE_NETWORK& net, QVector<WiFiNetworkInfo>& networks) {
    const QString ssid = ssidFromDot11Ssid(net.dot11Ssid);
    const bool isConnected = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;

    for (auto& info : networks) {
        if (info.ssid != ssid) {
            continue;
        }

        if (isConnected) {
            info.isConnected = true;
        }
        applyAuthAndEncryption(net, info);
    }
}

void applyAvailableNetworkList(const WLAN_AVAILABLE_NETWORK_LIST& netList,
                               QVector<WiFiNetworkInfo>& networks) {
    for (DWORD k = 0; k < netList.dwNumberOfItems; ++k) {
        applyAvailableNetwork(netList.Network[k], networks);
    }
}

void scanInterface(HANDLE handle,
                   const WLAN_INTERFACE_INFO& ifInfo,
                   bool triggerScan,
                   QVector<WiFiNetworkInfo>& networks) {
    if (triggerScan) {
        WlanScan(handle, &ifInfo.InterfaceGuid, nullptr, nullptr, nullptr);
        QThread::msleep(500);
    }

    PWLAN_BSS_LIST rawBssList = nullptr;
    DWORD result = WlanGetNetworkBssList(
        handle, &ifInfo.InterfaceGuid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &rawBssList);

    WlanPtr<WLAN_BSS_LIST> bssList(rawBssList, &wlanFreeMemory);
    if (result == ERROR_SUCCESS && bssList != nullptr) {
        appendBssNetworks(*bssList, networks);
    }

    PWLAN_AVAILABLE_NETWORK_LIST rawNetList = nullptr;
    result = WlanGetAvailableNetworkList(handle, &ifInfo.InterfaceGuid, 0, nullptr, &rawNetList);
    WlanPtr<WLAN_AVAILABLE_NETWORK_LIST> netList(rawNetList, &wlanFreeMemory);

    if (result == ERROR_SUCCESS && netList != nullptr) {
        applyAvailableNetworkList(*netList, networks);
    }
}

/// @brief Cached OUI database for vendor lookups
[[nodiscard]] const QHash<QString, QString>& ouiDatabase() {
    static QHash<QString, QString> db;
    static bool loaded = false;

    if (!loaded) {
        loaded = true;

        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates = {
            appDir + QStringLiteral("/resources/network/oui_database.txt"),
            appDir + QStringLiteral("/../resources/network/oui_database.txt"),
        };

        for (const auto& path : candidates) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&file);
                while (!in.atEnd()) {
                    const auto line = in.readLine().trimmed();
                    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                        continue;
                    }
                    // Expected format: "AA:BB:CC<tab>Vendor Name"
                    const auto parts = line.split(QLatin1Char('\t'));
                    if (parts.size() >= 2) {
                        db.insert(parts[0].toUpper(), parts[1]);
                    }
                }
                break;
            }
        }

        // Fallback common vendors
        if (db.isEmpty()) {
            db.insert(QStringLiteral("00:1A:2B"), QStringLiteral("Ayecom Technology"));
            db.insert(QStringLiteral("00:50:56"), QStringLiteral("VMware"));
            db.insert(QStringLiteral("00:0C:29"), QStringLiteral("VMware"));
            db.insert(QStringLiteral("B8:27:EB"), QStringLiteral("Raspberry Pi"));
            db.insert(QStringLiteral("DC:A6:32"), QStringLiteral("Raspberry Pi"));
            db.insert(QStringLiteral("00:25:00"), QStringLiteral("Apple"));
            db.insert(QStringLiteral("3C:22:FB"), QStringLiteral("Apple"));
            db.insert(QStringLiteral("AC:DE:48"), QStringLiteral("Apple"));
            db.insert(QStringLiteral("54:60:09"), QStringLiteral("Google"));
            db.insert(QStringLiteral("F4:F5:D8"), QStringLiteral("Google"));
            db.insert(QStringLiteral("30:B5:C2"), QStringLiteral("TP-Link"));
            db.insert(QStringLiteral("50:C7:BF"), QStringLiteral("TP-Link"));
            db.insert(QStringLiteral("2C:F0:5D"), QStringLiteral("Micro-Star"));
            db.insert(QStringLiteral("00:14:BF"), QStringLiteral("Linksys"));
            db.insert(QStringLiteral("C0:56:27"), QStringLiteral("Belkin"));
            db.insert(QStringLiteral("20:E5:2A"), QStringLiteral("NETGEAR"));
            db.insert(QStringLiteral("00:1F:33"), QStringLiteral("NETGEAR"));
            db.insert(QStringLiteral("F8:32:E4"), QStringLiteral("ASUSTek"));
            db.insert(QStringLiteral("04:D4:C4"), QStringLiteral("ASUSTek"));
            db.insert(QStringLiteral("00:1E:58"), QStringLiteral("D-Link"));
            db.insert(QStringLiteral("1C:7E:E5"), QStringLiteral("D-Link"));
            db.insert(QStringLiteral("E4:F0:42"), QStringLiteral("Google"));
            db.insert(QStringLiteral("3C:5A:B4"), QStringLiteral("Google"));
            db.insert(QStringLiteral("88:71:B1"), QStringLiteral("Intel"));
            db.insert(QStringLiteral("8C:EC:4B"), QStringLiteral("Dell"));
        }
    }
    return db;
}
}  // namespace

WiFiAnalyzer::WiFiAnalyzer(QObject* parent) : QObject(parent) {
    initializeWlan();
}

WiFiAnalyzer::~WiFiAnalyzer() {
    stopContinuousScan();
    cleanupWlan();
}

bool WiFiAnalyzer::initializeWlan() {
    if (m_wlanInitialized.load()) {
        return true;
    }

    DWORD negotiatedVersion = 0;
    HANDLE handle = nullptr;
    const DWORD result = WlanOpenHandle(kWlanClientVersion, nullptr, &negotiatedVersion, &handle);

    if (result != ERROR_SUCCESS) {
        return false;
    }

    m_wlanHandle = handle;
    m_wlanInitialized.store(true);
    return true;
}

void WiFiAnalyzer::cleanupWlan() {
    if (m_wlanHandle != nullptr) {
        WlanCloseHandle(m_wlanHandle, nullptr);
        m_wlanHandle = nullptr;
        m_wlanInitialized.store(false);
    }
}

bool WiFiAnalyzer::isWiFiAvailable() const {
    return m_wlanInitialized.load();
}

void WiFiAnalyzer::scan() {
    if (!m_wlanInitialized.load()) {
        Q_EMIT errorOccurred(QStringLiteral("WiFi adapter not available"));
        return;
    }

    auto networks = performWlanScan();
    m_lastScan = networks;

    Q_EMIT scanComplete(networks);

    auto channels = calculateChannelUtilization(networks);
    Q_EMIT channelUtilizationUpdated(channels);
}

void WiFiAnalyzer::startContinuousScan(int intervalMs) {
    Q_ASSERT(intervalMs >= 0);
    Q_ASSERT(m_scanTimer);
    stopContinuousScan();

    m_scanTimer = new QTimer(this);
    connect(m_scanTimer, &QTimer::timeout, this, [this]() {
        // Continuous scans skip WlanScan + Sleep to avoid blocking the GUI
        if (!m_wlanInitialized.load()) {
            return;
        }
        auto networks = performWlanScan(false);
        m_lastScan = networks;
        Q_EMIT scanComplete(networks);
        auto channels = calculateChannelUtilization(networks);
        Q_EMIT channelUtilizationUpdated(channels);
    });
    m_scanTimer->start(intervalMs);

    // Perform initial scan immediately
    scan();
}

void WiFiAnalyzer::stopContinuousScan() {
    if (m_scanTimer != nullptr) {
        m_scanTimer->stop();
        delete m_scanTimer;
        m_scanTimer = nullptr;
    }
}

QVector<WiFiNetworkInfo> WiFiAnalyzer::getLastScanResults() const {
    return m_lastScan;
}

QVector<WiFiNetworkInfo> WiFiAnalyzer::performWlanScan(bool triggerScan) {
    QVector<WiFiNetworkInfo> networks;

    if (m_wlanHandle == nullptr) {
        return networks;
    }

    auto handle = static_cast<HANDLE>(m_wlanHandle);

    PWLAN_INTERFACE_INFO_LIST rawIfList = nullptr;
    const DWORD result = WlanEnumInterfaces(handle, nullptr, &rawIfList);
    WlanPtr<WLAN_INTERFACE_INFO_LIST> ifList(rawIfList, &wlanFreeMemory);
    if (result != ERROR_SUCCESS || ifList == nullptr) {
        return networks;
    }

    for (DWORD i = 0; i < ifList->dwNumberOfItems; ++i) {
        scanInterface(handle, ifList->InterfaceInfo[i], triggerScan, networks);
    }
    return networks;
}

WiFiNetworkInfo WiFiAnalyzer::getCurrentConnection() const {
    // Return the connected network from last scan
    for (const auto& net : m_lastScan) {
        if (net.isConnected) {
            return net;
        }
    }
    return {};
}

int WiFiAnalyzer::frequencyToChannel(uint32_t freqKHz) {
    // 2.4 GHz band: channels 1-13 (2412 MHz to 2472 MHz, 5 MHz spacing)
    if (freqKHz >= kFreq2_4GHzBase && freqKHz < kFreq2_4GHzCh14) {
        return kChannel2_4GHzBase + static_cast<int>((freqKHz - kFreq2_4GHzBase) / kFreq2_4GHzStep);
    }

    // Channel 14 (Japan only): 2484 MHz
    if (freqKHz == kFreq2_4GHzCh14) {
        return kChannel14;
    }

    // 6 GHz band (5955 MHz – 7115 MHz): channels 1, 5, 9, ... with 5 MHz spacing
    if (freqKHz >= netdiag::kFreq6GHzStart) {
        return 1 + static_cast<int>((freqKHz - netdiag::kFreq6GHzStart) / kFreq2_4GHzStep);
    }

    // 5 GHz band
    if (freqKHz >= kFreq5GHzBase) {
        return static_cast<int>((freqKHz - kFreq5GHzBase) / kFreq2_4GHzStep);
    }

    return 0;
}

QString WiFiAnalyzer::frequencyToBand(uint32_t freqKHz) {
    if (freqKHz >= netdiag::kFreq6GHzStart && freqKHz <= netdiag::kFreq6GHzEnd) {
        return QStringLiteral("6 GHz");
    }
    if (freqKHz >= netdiag::kFreq5GHzStart && freqKHz <= netdiag::kFreq5GHzEnd) {
        return QStringLiteral("5 GHz");
    }
    if (freqKHz >= netdiag::kFreq2GHzStart && freqKHz <= netdiag::kFreq2GHzEnd) {
        return QStringLiteral("2.4 GHz");
    }
    return QStringLiteral("Unknown");
}

QString WiFiAnalyzer::lookupVendor(const QString& bssid) {
    if (bssid.length() < kMaxOuiPrefixLen) {
        return {};
    }

    // OUI is first 3 octets: "AA:BB:CC"
    const QString prefix = bssid.left(kMaxOuiPrefixLen).toUpper();
    const auto& db = ouiDatabase();
    auto it = db.find(prefix);
    if (it != db.end()) {
        return it.value();
    }
    return {};
}

QVector<WiFiChannelUtilization> WiFiAnalyzer::calculateChannelUtilization(
    const QVector<WiFiNetworkInfo>& networks) {
    QHash<int, WiFiChannelUtilization> channelMap;

    for (const auto& net : networks) {
        if (net.channelNumber <= 0) {
            continue;
        }

        auto& util = channelMap[net.channelNumber];
        util.channelNumber = net.channelNumber;
        util.band = net.band;
        util.networkCount++;
        util.ssids.append(net.ssid);
        util.averageSignalDbm += static_cast<double>(net.rssiDbm);
    }

    QVector<WiFiChannelUtilization> result;
    result.reserve(channelMap.size());

    for (auto it = channelMap.begin(); it != channelMap.end(); ++it) {
        auto& util = it.value();
        if (util.networkCount > 0) {
            util.averageSignalDbm /= static_cast<double>(util.networkCount);
        }
        // Interference score: number of networks * signal strength factor
        // Clamp to zero so very weak signals don't produce negative scores
        util.interferenceScore =
            static_cast<double>(util.networkCount) *
            std::max(0.0,
                     1.0 - (util.averageSignalDbm / static_cast<double>(netdiag::kSignalWeak)));
        result.append(util);
    }

    // Sort by channel number
    std::sort(result.begin(),
              result.end(),
              [](const WiFiChannelUtilization& a, const WiFiChannelUtilization& b) {
                  return a.channelNumber < b.channelNumber;
              });

    return result;
}

QString WiFiAnalyzer::formatMacAddress(const unsigned char* addr, int length) {
    return formatMacAddressString(addr, length);
}

}  // namespace sak
