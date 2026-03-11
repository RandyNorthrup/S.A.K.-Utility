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
/// Supports three scan levels (Safe/Moderate/Advanced) with increasing depth.
/// Classifies findings by risk level (Safe/Review/Risky) using pattern matching
/// and a protected-path list.
class LeftoverScanner {
public:
    explicit LeftoverScanner(const ProgramInfo& program, ScanLevel level);
    ~LeftoverScanner() = default;

    LeftoverScanner(const LeftoverScanner&) = delete;
    LeftoverScanner& operator=(const LeftoverScanner&) = delete;
    LeftoverScanner(LeftoverScanner&&) = default;
    LeftoverScanner& operator=(LeftoverScanner&&) = default;

    /// @brief Run the full leftover scan
    /// @param stopRequested Atomic flag for cancellation
    /// @param progressCallback Called with (currentPath, foundCount)
    [[nodiscard]] QVector<LeftoverItem> scan(
        const std::atomic<bool>& stopRequested,
        std::function<void(const QString&, int)> progressCallback = {});

private:
    ProgramInfo m_program;
    ScanLevel m_level;

    // Search patterns derived from program info
    QStringList m_namePatterns;       ///< Program name variations
    QStringList m_publisherPatterns;  ///< Publisher name variations
    QString m_installDirName;         ///< Last component of install path

    /// @brief Generate search patterns from program info
    void buildSearchPatterns();

    /// @brief Build name/install-dir patterns from display name
    void buildNamePatterns(const QSet<QString>& excludedWords);

    /// @brief Split text into words, filter, and append to target list
    void addFilteredWords(const QString& text,
                          const QString& split_pattern,
                          QStringList& target,
                          const QSet<QString>& excludes);

    // File system scanning
    QVector<LeftoverItem> scanFileSystem(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanDirectory(const QString& basePath,
                                        const std::atomic<bool>& stopRequested);

    // Registry scanning
#ifdef Q_OS_WIN
    QVector<LeftoverItem> scanRegistry(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanRegistryHive(HKEY hive,
                                           const QString& subkey,
                                           const QString& hiveName,
                                           const std::atomic<bool>& stopRequested);
#endif

    // System object scanning (Advanced level)
    QVector<LeftoverItem> scanServices(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanScheduledTasks(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanFirewallRules(const std::atomic<bool>& stopRequested);
    QVector<LeftoverItem> scanStartupEntries(const std::atomic<bool>& stopRequested);

    void scanFirewallDirection(const QStringList& netsh_args,
                               const QString& description,
                               const std::atomic<bool>& stopRequested,
                               QVector<LeftoverItem>& items);
    void scanRunKey(HKEY hive,
                    const wchar_t* subkey,
                    const QString& hive_name,
                    const std::atomic<bool>& stopRequested,
                    QVector<LeftoverItem>& items);
    void scanStartupFolder(const std::atomic<bool>& stopRequested, QVector<LeftoverItem>& items);

    // Safety classification
    LeftoverItem::RiskLevel classifyRisk(const QString& path, LeftoverItem::Type type) const;
    [[nodiscard]] bool isProtectedPath(const QString& path) const;
    [[nodiscard]] bool matchesProgram(const QString& name) const;

    // Protected system paths that should NEVER be deleted
    static const QStringList kProtectedPaths;

    /// @brief Calculate directory size recursively
    [[nodiscard]] static qint64 calculateSize(const QString& path);
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(!std::is_copy_constructible_v<LeftoverScanner>,
              "LeftoverScanner must not be copy-constructible.");

}  // namespace sak
