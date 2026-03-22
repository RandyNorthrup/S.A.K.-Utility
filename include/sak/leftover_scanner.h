// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file leftover_scanner.h
/// @brief Multi-level leftover scanning for orphaned files, folders, and
///        registry entries after program uninstallation

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QSet>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <type_traits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

/// @brief Scans for leftover files, folders, registry keys, services, and tasks
///
/// Uses a Revo-style targeted approach: scans the known install location,
/// checks exact-name matches in standard dirs, diffs registry snapshots,
/// and checks system objects at Advanced level. No substring word-matching.
class LeftoverScanner {
public:
    explicit LeftoverScanner(const ProgramInfo& program,
                             ScanLevel level,
                             const QSet<QString>& registryBefore = {});
    ~LeftoverScanner() = default;

    LeftoverScanner(const LeftoverScanner&) = delete;
    LeftoverScanner& operator=(const LeftoverScanner&) = delete;
    LeftoverScanner(LeftoverScanner&&) = default;
    LeftoverScanner& operator=(LeftoverScanner&&) = default;

    [[nodiscard]] QVector<LeftoverItem> scan(
        const std::atomic<bool>& stopRequested,
        std::function<void(const QString&, int)> progressCallback = {});

private:
    ProgramInfo m_program;
    ScanLevel m_level;
    QSet<QString> m_registryBefore;

    QStringList m_exactNames;     ///< Exact names for directory/file matching (lowercase)
    QString m_installDirName;     ///< Last component of install path (lowercase)
    QString m_installParentName;  ///< Parent dir of install path (lowercase)
    QString m_concatenatedName;   ///< Display name without separators (lowercase)

    void buildExactNames();

    // Phase 1: Remaining files at known install location
    QVector<LeftoverItem> scanInstallLocation(const std::atomic<bool>& stopRequested);

    // Phase 2: Known application directories
    QVector<LeftoverItem> scanKnownPaths(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanStandardDirs(const QString& basePath,
                                           const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanStandardFiles(const QString& basePath,
                                            const std::atomic<bool>& stopRequested);

    // Phase 3: Registry (snapshot diff + known paths)
#ifdef Q_OS_WIN
    QVector<LeftoverItem> scanRegistryDiff(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanKnownRegistryPaths(const std::atomic<bool>& stopRequested);
    [[nodiscard]] bool registryKeyMatchesProgram(const QString& keyPath) const;
#endif

    // Phase 4: System objects (Advanced only)
    QVector<LeftoverItem> scanServices(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanScheduledTasks(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanFirewallRules(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanStartupEntries(const std::atomic<bool>& stopRequested);

    void scanFirewallDirection(const QStringList& netsh_args,
                               const QString& description,
                               const std::atomic<bool>& stopRequested,
                               QVector<LeftoverItem>& items);
#ifdef Q_OS_WIN
    void scanRunKey(HKEY hive,
                    const wchar_t* subkey,
                    const QString& hive_name,
                    const std::atomic<bool>& stopRequested,
                    QVector<LeftoverItem>& items);
#endif
    void scanStartupFolder(const std::atomic<bool>& stopRequested, QVector<LeftoverItem>& items);

    // Classification and matching
    LeftoverItem::RiskLevel classifyRisk(const QString& path, LeftoverItem::Type type) const;
    LeftoverItem::RiskLevel classifyFileRisk(const QString& path_lower) const;
    LeftoverItem::RiskLevel classifyTypeRisk(LeftoverItem::Type type) const;
    void extractInstallDirNames();
    [[nodiscard]] bool isProtectedPath(const QString& path) const;
    [[nodiscard]] bool matchesProgramExact(const QString& nameLower) const;
    [[nodiscard]] bool matchesProgramStrict(const QString& text) const;
    [[nodiscard]] bool isPublisherDir(const QString& dirNameLower) const;

    static const QStringList kProtectedPaths;
    [[nodiscard]] static qint64 calculateSize(const QString& path);
};

// -- Compile-Time Invariants -------------------------------------------------

static_assert(!std::is_copy_constructible_v<LeftoverScanner>,
              "LeftoverScanner must not be copy-constructible.");

}  // namespace sak
