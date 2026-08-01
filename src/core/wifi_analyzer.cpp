// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_analyzer.cpp
/// @brief WiFi network scanning via Windows Native WiFi API

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "sak/wifi_analyzer.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

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
constexpr int kStrongSignalRssiDbm = -50;
constexpr int kMinimumSignalRssiDbm = -100;
constexpr int kRssiToQualityScale = 2;
constexpr int kMacAddressHexWidth = 2;
constexpr int kOuiDatabaseMinimumFields = 2;
constexpr int kForcedScanDelayMs = kTimerRetryBaseMs;

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
    if (rssiDbm >= kStrongSignalRssiDbm) {
        return kPercentMax;
    }
    if (rssiDbm <= kMinimumSignalRssiDbm) {
        return 0;
    }
    return kRssiToQualityScale * (rssiDbm - kMinimumSignalRssiDbm);
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
        mac += QStringLiteral("%1")
                   .arg(addr[i], kMacAddressHexWidth, kHexBase, QLatin1Char('0'))
                   .toUpper();
    }
    return mac;
}

// ── Per-BSSID security from 802.11 information elements ──────────────────────
constexpr int kIeIdRsn = 48;                     ///< RSN IE (WPA2/WPA3)
constexpr int kIeIdVendor = 221;                 ///< Vendor-specific IE (holds WPA1)
constexpr uint16_t kCapabilityPrivacy = 0x0010;  ///< Beacon capability Privacy bit
constexpr int kIeHeaderLen = 2;                  ///< element id + length octets
constexpr int kSuiteLen = 4;                     ///< OUI(3) + type(1)

[[nodiscard]] int readLe16(const unsigned char* p) {
    return static_cast<int>(p[0]) | (static_cast<int>(p[1]) << 8);
}

// WPA1 vendor IE payload begins with OUI 00:50:F2 followed by type 0x01.
[[nodiscard]] bool isWpaVendorIe(const unsigned char* d, int len) {
    return len >= kSuiteLen && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xF2 && d[3] == 0x01;
}

// Scan an RSN IE payload for the SAE AKM suite (00:0F:AC:08) => WPA3.
[[nodiscard]] bool rsnHasSae(const unsigned char* d, int len) {
    int pos = 2 + kSuiteLen;  // version(2) + group cipher suite(4)
    if (pos + 2 > len) {
        return false;
    }
    const int pairwise = readLe16(d + pos);
    pos += 2 + (kSuiteLen * pairwise);
    if (pos + 2 > len) {
        return false;
    }
    const int akm = readLe16(d + pos);
    pos += 2;
    for (int i = 0; i < akm; ++i) {
        const int off = pos + (kSuiteLen * i);
        if (off + kSuiteLen > len) {
            break;
        }
        if (d[off] == 0x00 && d[off + 1] == 0x0F && d[off + 2] == 0xAC && d[off + 3] == 0x08) {
            return true;
        }
    }
    return false;
}

struct IeSecurityScan {
    bool rsn = false;
    bool sae = false;
    bool wpa1 = false;
};

[[nodiscard]] IeSecurityScan scanSecurityIes(const unsigned char* ie, int ieLen) {
    IeSecurityScan scan;
    if (ie == nullptr) {
        return scan;
    }
    int pos = 0;
    while (pos + kIeHeaderLen <= ieLen) {
        const int id = ie[pos];
        const int len = ie[pos + 1];
        if (pos + kIeHeaderLen + len > ieLen) {
            break;
        }
        const unsigned char* data = ie + pos + kIeHeaderLen;
        if (id == kIeIdRsn) {
            scan.rsn = true;
            scan.sae = scan.sae || rsnHasSae(data, len);
        } else if (id == kIeIdVendor && isWpaVendorIe(data, len)) {
            scan.wpa1 = true;
        }
        pos += kIeHeaderLen + len;
    }
    return scan;
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

    // Authoritative per-BSSID security straight from THIS AP's beacon, so a rogue
    // AP cannot borrow a sibling BSSID's security label via a shared SSID.
    const auto* base = reinterpret_cast<const unsigned char*>(&bss);
    const bool privacy = (bss.usCapabilityInformation & kCapabilityPrivacy) != 0;
    const WiFiBssSecurity sec = WiFiAnalyzer::deriveBssSecurity(privacy,
                                                                base + bss.ulIeOffset,
                                                                static_cast<int>(bss.ulIeSize));
    info.authentication = sec.authentication;
    info.encryption = sec.encryption;
    info.isSecure = sec.isSecure;

    info.apVendor = WiFiAnalyzer::lookupVendor(info.bssid);
    return info;
}

void appendBssNetworks(const WLAN_BSS_LIST& bssList, QVector<WiFiNetworkInfo>& networks) {
    for (DWORD j = 0; j < bssList.dwNumberOfItems; ++j) {
        const auto& bss = bssList.wlanBssEntries[j];
        networks.append(networkFromBssEntry(bss));
    }
}

struct AuthEntry {
    DOT11_AUTH_ALGORITHM algo;
    const char* label;
};

static constexpr AuthEntry kAuthTable[] = {
    {DOT11_AUTH_ALGO_80211_OPEN, "Open"},
    {DOT11_AUTH_ALGO_80211_SHARED_KEY, "Shared Key"},
    {DOT11_AUTH_ALGO_WPA, "WPA-Enterprise"},
    {DOT11_AUTH_ALGO_WPA_PSK, "WPA-Personal"},
    {DOT11_AUTH_ALGO_RSNA, "WPA2-Enterprise"},
    {DOT11_AUTH_ALGO_RSNA_PSK, "WPA2-Personal"},
};

constexpr int kWpa3AuthThreshold = 9;

QString mapAuthAlgorithm(DOT11_AUTH_ALGORITHM algo) {
    for (const auto& entry : kAuthTable) {
        if (entry.algo == algo) {
            return QString::fromLatin1(entry.label);
        }
    }
    if (static_cast<int>(algo) >= kWpa3AuthThreshold) {
        return QStringLiteral("WPA3");
    }
    return QStringLiteral("Unknown");
}

struct CipherEntry {
    DOT11_CIPHER_ALGORITHM algo;
    const char* label;
    bool secure;
};

static constexpr CipherEntry kCipherTable[] = {
    {DOT11_CIPHER_ALGO_NONE, "None", false},
    {DOT11_CIPHER_ALGO_WEP40, "WEP", true},
    {DOT11_CIPHER_ALGO_WEP104, "WEP", true},
    {DOT11_CIPHER_ALGO_WEP, "WEP", true},
    {DOT11_CIPHER_ALGO_TKIP, "TKIP", true},
    {DOT11_CIPHER_ALGO_CCMP, "AES-CCMP", true},
};

void mapCipherAlgorithm(DOT11_CIPHER_ALGORITHM algo, QString& encryption, bool& is_secure) {
    for (const auto& entry : kCipherTable) {
        if (entry.algo == algo) {
            encryption = QString::fromLatin1(entry.label);
            is_secure = entry.secure;
            return;
        }
    }
    encryption = QStringLiteral("Other");
    is_secure = true;
}

void applyAuthAndEncryption(const WLAN_AVAILABLE_NETWORK& net, WiFiNetworkInfo& info) {
    // info.isSecure was already set per-BSSID from this AP's own beacon. The
    // available-network entry is an SSID-wide aggregate, so only adopt its
    // (typically richer) auth/cipher labels when the aggregate agrees with this
    // BSSID's security class -- never let it flip isSecure. Otherwise an Open
    // evil-twin sharing a secured SSID would inherit the secure label.
    QString aggEncryption;
    bool aggSecure = false;
    mapCipherAlgorithm(net.dot11DefaultCipherAlgorithm, aggEncryption, aggSecure);
    if (aggSecure != info.isSecure) {
        return;
    }
    info.authentication = mapAuthAlgorithm(net.dot11DefaultAuthAlgorithm);
    info.encryption = aggEncryption;
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

// Scan one interface. @p readOk is set true if AT LEAST ONE of the two list reads succeeded
// (so an empty result is a genuine "no networks"); it stays false only when BOTH reads errored
// -- e.g. the radio is powered off (ERROR_NDIS_DOT11_POWER_STATE_INVALID) -- which the caller
// surfaces as an honest scan failure rather than a misleading empty success.
constexpr int kScanCompleteTimeoutMs = 4000;  // upper bound on waiting for a scan to finish

void CALLBACK onWlanScanNotification(PWLAN_NOTIFICATION_DATA data, PVOID context) {
    if (context == nullptr || data == nullptr) {
        return;
    }
    if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM &&
        (data->NotificationCode == wlan_notification_acm_scan_complete ||
         data->NotificationCode == wlan_notification_acm_scan_fail)) {
        SetEvent(static_cast<HANDLE>(context));
    }
}

// Trigger a scan and wait (bounded) for the driver's ACM scan-complete/scan-fail
// notification instead of blindly sleeping a fixed interval -- a fixed sleep may be
// too short (the BSS list read then returns STALE cached results) or wastefully long.
// Falls back to the fixed sleep only if WlanScan or the notification registration fails.
void triggerScanAndWait(HANDLE handle, const GUID& guid) {
    HANDLE scanDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD prevSource = 0;
    const bool registered = scanDone != nullptr &&
                            WlanRegisterNotification(handle,
                                                     WLAN_NOTIFICATION_SOURCE_ACM,
                                                     FALSE,
                                                     &onWlanScanNotification,
                                                     scanDone,
                                                     nullptr,
                                                     &prevSource) == ERROR_SUCCESS;

    const DWORD scanStatus = WlanScan(handle, &guid, nullptr, nullptr, nullptr);
    if (scanStatus == ERROR_SUCCESS && registered) {
        WaitForSingleObject(scanDone, kScanCompleteTimeoutMs);
    } else {
        QThread::msleep(kForcedScanDelayMs);
    }

    if (registered) {
        WlanRegisterNotification(
            handle, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, &prevSource);
    }
    if (scanDone != nullptr) {
        CloseHandle(scanDone);
    }
}

void scanInterface(HANDLE handle,
                   const WLAN_INTERFACE_INFO& ifInfo,
                   bool triggerScan,
                   QVector<WiFiNetworkInfo>& networks,
                   bool& readOk) {
    if (triggerScan) {
        triggerScanAndWait(handle, ifInfo.InterfaceGuid);
    }

    readOk = false;

    PWLAN_BSS_LIST rawBssList = nullptr;
    DWORD result = WlanGetNetworkBssList(
        handle, &ifInfo.InterfaceGuid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &rawBssList);

    WlanPtr<WLAN_BSS_LIST> bssList(rawBssList, &wlanFreeMemory);
    if (result == ERROR_SUCCESS && bssList != nullptr) {
        appendBssNetworks(*bssList, networks);
        readOk = true;
    }

    PWLAN_AVAILABLE_NETWORK_LIST rawNetList = nullptr;
    result = WlanGetAvailableNetworkList(handle, &ifInfo.InterfaceGuid, 0, nullptr, &rawNetList);
    WlanPtr<WLAN_AVAILABLE_NETWORK_LIST> netList(rawNetList, &wlanFreeMemory);

    if (result == ERROR_SUCCESS && netList != nullptr) {
        applyAvailableNetworkList(*netList, networks);
        readOk = true;
    }
}

// Parse the OUI text database ("AA:BB:CC<tab>Vendor") from the first candidate
// path that opens; leaves @p db untouched if none is present.
void loadOuiDatabaseFile(QHash<QString, QString>& db) {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/resources/network/oui_database.txt"),
        appDir + QStringLiteral("/../resources/network/oui_database.txt"),
    };

    for (const auto& path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        QTextStream in(&file);
        while (!in.atEnd()) {
            const auto line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            const auto parts = line.split(QLatin1Char('\t'));
            if (parts.size() >= kOuiDatabaseMinimumFields) {
                db.insert(parts[0].toUpper(), parts[1]);
            }
        }
        return;
    }
}

// Minimal built-in vendor set used only when no OUI text database ships.
void seedFallbackVendors(QHash<QString, QString>& db) {
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

/// @brief Cached OUI database for vendor lookups.
///
/// The table is built once into a function-local static; C++11 guarantees the
/// initializer runs exactly once even under concurrent first calls, so parallel
/// scans can no longer race the previous hand-rolled `loaded` flag / QHash fill.
[[nodiscard]] const QHash<QString, QString>& ouiDatabase() {
    static const QHash<QString, QString> db = [] {
        QHash<QString, QString> table;
        loadOuiDatabaseFile(table);
        if (table.isEmpty()) {
            seedFallbackVendors(table);
        }
        return table;
    }();
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
        Q_EMIT scanComplete({});
        return;
    }

    bool scanError = false;
    auto networks = performWlanScan(true, scanError);
    m_lastScan = networks;

    // Fail CLOSED: a driver/radio failure that yields NO data (e.g. the radio is off) is reported
    // honestly via errorOccurred rather than as a misleading "0 networks" success. A partial scan
    // (some interface produced data) is still a success.
    if (scanError && networks.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral(
            "WiFi scan failed (no data from the adapter -- the radio may be off or there is no "
            "wireless interface)"));
        Q_EMIT scanComplete(networks);
        return;
    }

    Q_EMIT scanComplete(networks);

    auto channels = calculateChannelUtilization(networks);
    Q_EMIT channelUtilizationUpdated(channels);
}

void WiFiAnalyzer::startContinuousScan(int intervalMs) {
    if (intervalMs < 0) {
        sak::logWarning("WiFiAnalyzer::startContinuousScan: ignoring negative interval {}",
                        intervalMs);
        return;
    }
    stopContinuousScan();

    m_scanTimer = new QTimer(this);
    connect(m_scanTimer, &QTimer::timeout, this, [this]() {
        // Continuous scans skip WlanScan + Sleep to avoid blocking the GUI
        if (!m_wlanInitialized.load()) {
            return;
        }
        bool scanError = false;  // background refresh: a transient read error just yields no update
        auto networks = performWlanScan(false, scanError);
        if (scanError && networks.isEmpty()) {
            // Transient driver/radio failure -> keep the last good scan rather than
            // clobbering it with an empty list and emitting a misleading "0 networks".
            return;
        }
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

QVector<WiFiNetworkInfo> WiFiAnalyzer::performWlanScan(bool triggerScan, bool& scanError) {
    scanError = false;
    QVector<WiFiNetworkInfo> networks;

    if (m_wlanHandle == nullptr) {
        scanError = true;
        return networks;
    }

    auto handle = static_cast<HANDLE>(m_wlanHandle);

    PWLAN_INTERFACE_INFO_LIST rawIfList = nullptr;
    const DWORD result = WlanEnumInterfaces(handle, nullptr, &rawIfList);
    WlanPtr<WLAN_INTERFACE_INFO_LIST> ifList(rawIfList, &wlanFreeMemory);
    if (result != ERROR_SUCCESS || ifList == nullptr || ifList->dwNumberOfItems == 0) {
        scanError = true;  // no wireless interface to scan (enum failed or zero adapters)
        return networks;
    }

    bool anyInterfaceOk = false;
    for (DWORD i = 0; i < ifList->dwNumberOfItems; ++i) {
        bool interfaceOk = false;
        scanInterface(handle, ifList->InterfaceInfo[i], triggerScan, networks, interfaceOk);
        anyInterfaceOk = anyInterfaceOk || interfaceOk;
    }
    // Every interface's list reads errored (e.g. radio off) -> the scan FAILED; distinguish that
    // from a genuine empty result (a read succeeded with zero networks).
    scanError = !anyInterfaceOk;
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

    // 6 GHz band (5955 MHz - 7115 MHz): channels 1, 5, 9, ... with 5 MHz spacing
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

WiFiBssSecurity WiFiAnalyzer::deriveBssSecurity(bool privacyBit,
                                                const unsigned char* ie,
                                                int ieLen) {
    WiFiBssSecurity sec;
    const IeSecurityScan scan = scanSecurityIes(ie, ieLen);
    if (scan.rsn) {
        sec.authentication = scan.sae ? QStringLiteral("WPA3") : QStringLiteral("WPA2");
        sec.encryption = QStringLiteral("AES-CCMP");
        sec.isSecure = true;
        return sec;
    }
    if (scan.wpa1) {
        sec.authentication = QStringLiteral("WPA");
        sec.encryption = QStringLiteral("TKIP");
        sec.isSecure = true;
        return sec;
    }
    if (privacyBit) {
        // Privacy bit set but no RSN/WPA IE => legacy WEP (encrypted, but weak).
        sec.authentication = QStringLiteral("WEP");
        sec.encryption = QStringLiteral("WEP");
        sec.isSecure = true;
        return sec;
    }
    sec.authentication = QStringLiteral("Open");
    sec.encryption = QStringLiteral("None");
    sec.isSecure = false;
    return sec;
}

QVector<WiFiChannelUtilization> WiFiAnalyzer::calculateChannelUtilization(
    const QVector<WiFiNetworkInfo>& networks) {
    // Key by band+channel: 2.4/5/6 GHz reuse channel numbers (e.g. 6 GHz ch 1 vs 2.4 GHz ch 1),
    // so a channel-only key merges different bands into one bogus entry. frequencyToBand only
    // returns "2.4 GHz"/"5 GHz"/"6 GHz"/"Unknown", none containing '|', so the delimiter is safe.
    QHash<QString, WiFiChannelUtilization> channelMap;

    for (const auto& net : networks) {
        if (net.channelNumber <= 0) {
            continue;
        }

        const QString key = net.band + QLatin1Char('|') + QString::number(net.channelNumber);
        auto& util = channelMap[key];
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

    // Sort by channel number, then band so entries sharing a channel across bands are deterministic
    std::sort(result.begin(),
              result.end(),
              [](const WiFiChannelUtilization& a, const WiFiChannelUtilization& b) {
                  if (a.channelNumber != b.channelNumber) {
                      return a.channelNumber < b.channelNumber;
                  }
                  return a.band < b.band;
              });

    return result;
}

QString WiFiAnalyzer::formatMacAddress(const unsigned char* addr, int length) {
    return formatMacAddressString(addr, length);
}

}  // namespace sak
