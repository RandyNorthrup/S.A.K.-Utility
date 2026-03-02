// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_types.h
/// @brief Shared data types for the Advanced Uninstall Panel

#pragma once

#include <QDateTime>
#include <QImage>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <type_traits>

namespace sak {

// ── ProgramInfo ─────────────────────────────────────────────────────────────

/// @brief Comprehensive information about an installed program
struct ProgramInfo {
    // Identity
    QString displayName;
    QString publisher;
    QString displayVersion;
    QString installDate;        ///< YYYYMMDD or localized

    // Paths
    QString installLocation;
    QString uninstallString;
    QString quietUninstallString;
    QString modifyPath;
    QString displayIcon;        ///< Path to icon resource

    // Registry
    QString registryKeyPath;    ///< Full registry key path

    // Metadata
    qint64 estimatedSizeKB = 0; ///< From registry EstimatedSize
    qint64 actualSizeBytes = 0; ///< Calculated disk usage

    // Classification
    enum class Source {
        RegistryHKLM,
        RegistryHKLM_WOW64,
        RegistryHKCU,
        UWP,
        Provisioned
    };
    Source source = Source::RegistryHKLM;

    // UWP-specific
    QString packageFamilyName;
    QString packageFullName;

    // Status
    bool isSystemComponent = false;
    bool isOrphaned = false;
    bool isBloatware = false;

    // Icon cache — stored as QImage for thread safety (QPixmap is GUI-thread only)
    QImage cachedImage;
};

// ── ScanLevel ───────────────────────────────────────────────────────────────

/// @brief Scan level for leftover detection
enum class ScanLevel {
    Safe,       ///< Only obvious leftovers in known locations (fast)
    Moderate,   ///< Extended scanning with pattern matching (recommended)
    Advanced    ///< Deep scan including services, tasks, firewall, shell extensions
};

// ── LeftoverItem ────────────────────────────────────────────────────────────

/// @brief A single leftover item found after uninstallation
struct LeftoverItem {
    enum class Type {
        File,
        Folder,
        RegistryKey,
        RegistryValue,
        Service,
        ScheduledTask,
        FirewallRule,
        StartupEntry,
        ShellExtension
    };

    enum class RiskLevel {
        Safe,       ///< Green — clearly belongs to the uninstalled app
        Review,     ///< Yellow — likely belongs, but shared component possible
        Risky       ///< Red — may be shared or system-related
    };

    Type type = Type::File;
    RiskLevel risk = RiskLevel::Safe;
    QString path;
    QString description;
    qint64 sizeBytes = 0;
    bool selected = false;

    // Registry-specific
    QString registryValueName;
    QString registryValueData;
};

// ── UninstallReport ─────────────────────────────────────────────────────────

/// @brief Result of uninstall + leftover scan + cleanup pipeline
struct UninstallReport {
    QString programName;
    QString programVersion;
    QString programPublisher;

    // Timing
    QDateTime startTime;
    QDateTime endTime;

    // Restore
    bool restorePointCreated = false;
    QString restorePointName;

    // Uninstall phase
    enum class UninstallResult {
        Success,
        Failed,
        Cancelled,
        Skipped     ///< Forced uninstall — no native uninstaller run
    };
    UninstallResult uninstallResult = UninstallResult::Success;
    int nativeExitCode = 0;

    // Leftover scan phase
    ScanLevel scanLevel = ScanLevel::Moderate;
    QVector<LeftoverItem> foundLeftovers;

    // Cleanup phase
    int filesDeleted = 0;
    int foldersDeleted = 0;
    int registryKeysDeleted = 0;
    int registryValuesDeleted = 0;
    int servicesRemoved = 0;
    int tasksRemoved = 0;
    int firewallRulesRemoved = 0;
    int startupEntriesRemoved = 0;
    int failedDeletions = 0;
    qint64 totalSpaceRecovered = 0;

    QStringList errorLog;
};

// ── UninstallQueueItem ──────────────────────────────────────────────────────

/// @brief Batch uninstall queue item
struct UninstallQueueItem {
    ProgramInfo program;
    ScanLevel scanLevel = ScanLevel::Moderate;
    bool autoCleanSafeLeftovers = true;

    enum class Status {
        Queued,
        InProgress,
        Completed,
        Failed,
        Cancelled
    };
    Status status = Status::Queued;
    UninstallReport report;
};

// ── ViewFilter ──────────────────────────────────────────────────────────────

/// @brief View filter presets for the program list
enum class ViewFilter {
    All = 0,
    Win32Only,
    UwpOnly,
    BloatwareOnly,
    OrphanedOnly
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_default_constructible_v<ProgramInfo>);
static_assert(std::is_copy_constructible_v<ProgramInfo>);
static_assert(std::is_default_constructible_v<LeftoverItem>);
static_assert(std::is_copy_constructible_v<LeftoverItem>);
static_assert(std::is_default_constructible_v<UninstallReport>);
static_assert(std::is_copy_constructible_v<UninstallReport>);
static_assert(std::is_default_constructible_v<UninstallQueueItem>);
static_assert(std::is_copy_constructible_v<UninstallQueueItem>);

} // namespace sak

Q_DECLARE_METATYPE(sak::ProgramInfo)
Q_DECLARE_METATYPE(sak::LeftoverItem)
Q_DECLARE_METATYPE(sak::UninstallReport)
Q_DECLARE_METATYPE(QVector<sak::ProgramInfo>)
Q_DECLARE_METATYPE(QVector<sak::LeftoverItem>)
