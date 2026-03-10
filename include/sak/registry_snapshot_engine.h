// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file registry_snapshot_engine.h
/// @brief Registry key snapshot capture and diff for leftover detection

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

/// @brief Captures a snapshot of registry keys under monitored paths and diffs them
///
/// Used before and after uninstallation to identify registry keys that survived
/// the native uninstaller — potential leftovers that should be cleaned.
class RegistrySnapshotEngine {
public:
    RegistrySnapshotEngine() = default;
    ~RegistrySnapshotEngine() = default;

    RegistrySnapshotEngine(const RegistrySnapshotEngine&) = delete;
    RegistrySnapshotEngine& operator=(const RegistrySnapshotEngine&) = delete;
    RegistrySnapshotEngine(RegistrySnapshotEngine&&) = default;
    RegistrySnapshotEngine& operator=(RegistrySnapshotEngine&&) = default;

    /// @brief Capture a snapshot of registry keys under monitored paths
    /// @return Set of full key paths (e.g., "HKLM\\SOFTWARE\\CompanyName\\Product")
    [[nodiscard]] static QSet<QString> captureSnapshot();

    /// @brief Diff two snapshots to find potential leftover keys
    /// @param before Snapshot before uninstall
    /// @param after Snapshot after uninstall
    /// @param programNamePatterns Patterns to match against key names
    /// @return Leftover items for keys that match program patterns
    [[nodiscard]] static QVector<LeftoverItem> diffSnapshots(
        const QSet<QString>& before,
        const QSet<QString>& after,
        const QStringList& programNamePatterns);

private:
#ifdef Q_OS_WIN
    /// @brief Enumerate all subkeys under a registry path
    static void enumerateKeys(HKEY hive,
                              const QString& subkey,
                              const QString& hiveName,
                              QSet<QString>& output,
                              int maxDepth = 3);
#endif

    /// @brief Monitored registry paths for snapshots
    static const QStringList kMonitoredPaths;

    /// @brief Maximum enumeration depth to limit scan time
    static constexpr int kDefaultMaxDepth = 3;
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(!std::is_copy_constructible_v<RegistrySnapshotEngine>,
              "RegistrySnapshotEngine must not be copy-constructible.");

}  // namespace sak
