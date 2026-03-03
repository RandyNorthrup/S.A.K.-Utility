// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_adapter_inspector.h
/// @brief Enumerates network adapters via Windows IP Helper API

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief Enumerates all network adapters with complete configuration details
///
/// Uses GetAdaptersAddresses (IP_ADAPTER_ADDRESSES) for comprehensive
/// adapter enumeration including IPv4/IPv6 addresses, DNS servers,
/// DHCP config, link speed, and driver info.
class NetworkAdapterInspector : public QObject {
    Q_OBJECT

public:
    explicit NetworkAdapterInspector(QObject* parent = nullptr);
    ~NetworkAdapterInspector() override = default;

    NetworkAdapterInspector(const NetworkAdapterInspector&) = delete;
    NetworkAdapterInspector& operator=(const NetworkAdapterInspector&) = delete;
    NetworkAdapterInspector(NetworkAdapterInspector&&) = delete;
    NetworkAdapterInspector& operator=(NetworkAdapterInspector&&) = delete;

    /// @brief Enumerate all adapters (blocking — run on worker thread)
    void scan();

    /// @brief Re-enumerate adapters (alias for scan)
    void refresh();

    /// @brief Format link speed to human-readable string
    [[nodiscard]] static QString formatLinkSpeed(uint64_t bps);

    /// @brief Format MAC address bytes to colon-separated string
    [[nodiscard]] static QString formatMacAddress(const unsigned char* addr,
                                                   unsigned long length);

Q_SIGNALS:
    void scanComplete(QVector<sak::NetworkAdapterInfo> adapters);
    void errorOccurred(QString error);

private:
    [[nodiscard]] QVector<NetworkAdapterInfo> enumerateAdapters();
};

} // namespace sak

static_assert(!std::is_copy_constructible_v<sak::NetworkAdapterInspector>,
    "NetworkAdapterInspector must not be copyable.");
