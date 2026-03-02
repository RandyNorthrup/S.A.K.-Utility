// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file program_enumerator.h
/// @brief Enumerates all installed Win32 and UWP programs with rich metadata

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QObject>
#include <QVector>

#include <memory>
#include <type_traits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

/// @brief Enumerates installed programs from registry (HKLM/HKCU/WOW64) and UWP
///
/// Provides comprehensive program listing with metadata including display name,
/// publisher, version, size, install path, uninstall command, icon, and
/// classification (system component, orphaned, bloatware).
class ProgramEnumerator : public QObject {
    Q_OBJECT

public:
    explicit ProgramEnumerator(QObject* parent = nullptr);
    ~ProgramEnumerator() override;

    ProgramEnumerator(const ProgramEnumerator&) = delete;
    ProgramEnumerator& operator=(const ProgramEnumerator&) = delete;
    ProgramEnumerator(ProgramEnumerator&&) = delete;
    ProgramEnumerator& operator=(ProgramEnumerator&&) = delete;

    /// @brief Start async enumeration of all installed programs
    void enumerateAll();

    /// @brief Get the last enumeration result (cached)
    [[nodiscard]] QVector<ProgramInfo> programs() const;

    /// @brief Detect orphaned entries (install path or uninstaller missing)
    void detectOrphaned(QVector<ProgramInfo>& programs);

    /// @brief Mark known bloatware using pattern database
    void markBloatware(QVector<ProgramInfo>& programs);

    /// @brief Calculate actual disk usage for a program's install directory
    [[nodiscard]] static qint64 calculateDirSize(const QString& path);

Q_SIGNALS:
    void enumerationStarted();
    void enumerationProgress(int current, int total);
    void enumerationFinished(QVector<ProgramInfo> programs);
    void enumerationFailed(const QString& error);

private:
#ifdef Q_OS_WIN
    /// @brief Scan Win32 programs from all registry hives
    QVector<ProgramInfo> scanRegistryPrograms();

    /// @brief Scan a single registry hive's Uninstall key
    QVector<ProgramInfo> scanRegistryHive(HKEY hive, const wchar_t* subkey,
                                          ProgramInfo::Source source);

    /// @brief Read a single registry string value
    [[nodiscard]] static QString readRegString(HKEY key, const wchar_t* valueName);

    /// @brief Read a single registry DWORD value
    [[nodiscard]] static DWORD readRegDword(HKEY key, const wchar_t* valueName);

    /// @brief Check if entry is a system component (filter out)
    [[nodiscard]] static bool isSystemComponent(HKEY key);
#endif

    /// @brief Scan UWP/AppX packages via PowerShell
    QVector<ProgramInfo> scanUwpPackages();

    /// @brief Scan provisioned (all-users) UWP packages
    QVector<ProgramInfo> scanProvisionedPackages();

    /// @brief Extract icon image from executable file
    [[nodiscard]] static QImage extractIcon(const QString& path);

    /// @brief Deduplicate programs from multiple sources
    void deduplicatePrograms(QVector<ProgramInfo>& programs);

#ifdef Q_OS_WIN
    // Registry paths
    static constexpr const wchar_t* kUninstallKey64 =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    static constexpr const wchar_t* kUninstallKeyWow64 =
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    static constexpr const wchar_t* kUninstallKeyHKCU =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
#endif

    QVector<ProgramInfo> m_cachedPrograms;
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_base_of_v<QObject, ProgramEnumerator>,
    "ProgramEnumerator must inherit QObject.");
static_assert(!std::is_copy_constructible_v<ProgramEnumerator>,
    "ProgramEnumerator must not be copy-constructible.");

} // namespace sak
