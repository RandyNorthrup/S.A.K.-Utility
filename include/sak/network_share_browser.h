// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_share_browser.h
/// @brief SMB share discovery and access testing

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>
#include <QPair>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief Network share discovery and access tester
///
/// Uses NetShareEnum (netapi32.dll) to enumerate SMB shares
/// on a target host and tests read/write access.
class NetworkShareBrowser : public QObject {
    Q_OBJECT

public:
    explicit NetworkShareBrowser(QObject* parent = nullptr);
    ~NetworkShareBrowser() override = default;

    NetworkShareBrowser(const NetworkShareBrowser&) = delete;
    NetworkShareBrowser& operator=(const NetworkShareBrowser&) = delete;
    NetworkShareBrowser(NetworkShareBrowser&&) = delete;
    NetworkShareBrowser& operator=(NetworkShareBrowser&&) = delete;

    /// @brief Discover shares on a host (blocking)
    void discoverShares(const QString& hostname);

    /// @brief Test read/write access to a UNC path (blocking)
    void testAccess(const QString& uncPath);

    void cancel();

Q_SIGNALS:
    void shareDiscovered(sak::NetworkShareInfo share);
    void discoveryComplete(QVector<sak::NetworkShareInfo> shares);
    void accessTestComplete(QString uncPath, bool canRead, bool canWrite);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    [[nodiscard]] QVector<NetworkShareInfo> enumerateShares(const QString& hostname);
    [[nodiscard]] QPair<bool, bool> testReadWriteAccess(const QString& uncPath);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::NetworkShareBrowser>,
              "NetworkShareBrowser must not be copyable.");
