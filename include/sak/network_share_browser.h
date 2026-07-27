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

    /// @brief Enumerate shares on a host read-only (blocking): performs NO write-access probe,
    /// so nothing is written to any discovered share. The per-share access fields
    /// (canRead/canWrite) are therefore left unset. An empty hostname (or "localhost"/loopback)
    /// enumerates the LOCAL machine's shares via a pure local API call (no SMB round-trip, no
    /// credential exposure). Used by headless callers that must not mutate the target.
    /// @param hostname target host (empty = local machine)
    /// @param ok set false only if the NetShareEnum call itself failed (vs a genuine empty list)
    /// @return discovered shares (access fields NOT populated)
    [[nodiscard]] QVector<NetworkShareInfo> listSharesReadOnly(const QString& hostname, bool& ok);

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

    [[nodiscard]] QVector<NetworkShareInfo> enumerateShares(const QString& hostname,
                                                            bool testAccess,
                                                            bool& ok);
    [[nodiscard]] QPair<bool, bool> testReadWriteAccess(const QString& uncPath);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::NetworkShareBrowser>,
              "NetworkShareBrowser must not be copyable.");
