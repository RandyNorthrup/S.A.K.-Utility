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

class TestNetworkShareBrowser;

namespace sak {

/// @brief Network share discovery and access tester
///
/// Uses NetShareEnum (netapi32.dll) to enumerate SMB shares
/// on a target host and tests read/write access.
class NetworkShareBrowser : public QObject {
    Q_OBJECT

    // Unit-test seam: exercise the terminal-signal decision (emitDiscoveryOutcome) directly, so
    // the fail-closed split between a COMPLETE enumeration and a partial/failed one is provable
    // without a live SMB host. The test class lives in the global namespace, so befriend
    // ::TestNetworkShareBrowser (not sak::TestNetworkShareBrowser).
    friend class ::TestNetworkShareBrowser;

public:
    explicit NetworkShareBrowser(QObject* parent = nullptr);
    ~NetworkShareBrowser() override = default;

    NetworkShareBrowser(const NetworkShareBrowser&) = delete;
    NetworkShareBrowser& operator=(const NetworkShareBrowser&) = delete;
    NetworkShareBrowser(NetworkShareBrowser&&) = delete;
    NetworkShareBrowser& operator=(NetworkShareBrowser&&) = delete;

    /// @brief Discover shares on a host (blocking)
    ///
    /// Emits shareDiscovered per share, then EXACTLY ONE terminal signal: discoveryComplete
    /// when the enumeration was authoritative, discoveryFailed when it was not (bad request,
    /// NetShareEnum error, or a cancel that truncated the resume-handle loop). A partial list
    /// is never delivered through discoveryComplete.
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
    /// The enumeration was AUTHORITATIVE: every share the host reported is in @p shares.
    void discoveryComplete(QVector<sak::NetworkShareInfo> shares);
    /// The enumeration did NOT complete, so @p partialShares is whatever was read before the
    /// failure/cancel and carries no wholeness claim. Emitted INSTEAD of discoveryComplete so a
    /// truncated read can never be consumed as a finished discovery.
    void discoveryFailed(QVector<sak::NetworkShareInfo> partialShares, QString reason);
    void accessTestComplete(QString uncPath, bool canRead, bool canWrite);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    [[nodiscard]] QVector<NetworkShareInfo> enumerateShares(const QString& hostname,
                                                            bool testAccess,
                                                            bool& ok);
    /// Emit the single terminal signal for one discovery run: discoveryComplete when @p ok,
    /// discoveryFailed (with the partial list and a reason naming cancel vs enumeration error)
    /// otherwise. Never emits both.
    void emitDiscoveryOutcome(const QVector<NetworkShareInfo>& shares,
                              bool ok,
                              const QString& displayHost);
    /// Append one NetShareEnum result buffer to @p shares (probing access when asked).
    /// @p shareInfoBuffer is a PSHARE_INFO_1 passed as void* to keep the Windows LAN
    /// Manager headers out of this public header.
    void appendSharesFromBuffer(const void* shareInfoBuffer,
                                unsigned long entriesRead,
                                const QString& displayHost,
                                bool testAccess,
                                QVector<NetworkShareInfo>& shares);
    [[nodiscard]] QPair<bool, bool> testReadWriteAccess(const QString& uncPath);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::NetworkShareBrowser>,
              "NetworkShareBrowser must not be copyable.");
