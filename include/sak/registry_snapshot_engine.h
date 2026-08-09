// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file registry_snapshot_engine.h
/// @brief Registry key snapshot capture for leftover detection

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QSet>
#include <QStringList>
#include <QVector>

#include <type_traits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

/// @brief Captures a snapshot of registry keys under monitored paths
///
/// Used before and after uninstallation to identify registry keys that survived
/// the native uninstaller -- potential leftovers that should be cleaned. The
/// before/after diff is performed by the caller (LeftoverScanner::scanRegistryDiff).
class RegistrySnapshotEngine {
public:
    RegistrySnapshotEngine() = default;
    ~RegistrySnapshotEngine() = default;

    RegistrySnapshotEngine(const RegistrySnapshotEngine&) = delete;
    RegistrySnapshotEngine& operator=(const RegistrySnapshotEngine&) = delete;
    RegistrySnapshotEngine(RegistrySnapshotEngine&&) = default;
    RegistrySnapshotEngine& operator=(RegistrySnapshotEngine&&) = default;

    /// @brief Capture a snapshot of registry keys under monitored paths
    ///
    /// The walk is deliberately bounded, and the bounds are part of the contract:
    /// kDefaultMaxDepth levels below each monitored root, a per-key child budget, a total
    /// key budget, and HKLM\\SOFTWARE\\Classes excluded outright. Keys below the depth cap
    /// are out of scope for leftover detection rather than a failure; hitting a breadth or
    /// total budget IS a failure, because real keys were then dropped. Registry symbolic
    /// links (REG_LINK) are refused, never followed.
    ///
    /// @param reliable Optional out-param. Set FALSE when any monitored subtree could
    ///        not be fully enumerated (open/query/enum failure, a refused REG_LINK, or a
    ///        breadth budget) so a PARTIAL snapshot is never consumed as an authoritative,
    ///        complete one. TRUE means every key within the bounds above was captured.
    ///        Null preserves the caller not caring -- any caller that acts on the diff
    ///        must pass it and honor it.
    /// @return Set of full key paths (e.g., "HKLM\\SOFTWARE\\CompanyName\\Product")
    [[nodiscard]] static QSet<QString> captureSnapshot(bool* reliable = nullptr);

private:
#ifdef Q_OS_WIN
    static constexpr int kDefaultMaxDepth = 3;

    /// @brief Accumulator threaded through the recursive walk: the captured key set
    ///        plus the reliability flag that any enumeration failure clears.
    struct SnapshotSink {
        QSet<QString>& keys;
        bool& reliable;
    };

    /// @brief Enumerate all subkeys under a registry path
    static void enumerateKeys(HKEY hive,
                              const QString& subkey,
                              const QString& hiveName,
                              SnapshotSink sink,
                              int maxDepth = kDefaultMaxDepth);
#endif

    /// @brief Monitored registry paths for snapshots
    static const QStringList kMonitoredPaths;
};

// -- Compile-Time Invariants -------------------------------------------------

static_assert(!std::is_copy_constructible_v<RegistrySnapshotEngine>,
              "RegistrySnapshotEngine must not be copy-constructible.");

}  // namespace sak
