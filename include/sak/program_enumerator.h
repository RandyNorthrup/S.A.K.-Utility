// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file program_enumerator.h
/// @brief Enumerates all installed Win32 and UWP programs with rich metadata

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/process_runner.h"

#include <QObject>
#include <QVector>

#include <atomic>
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

    /// @brief Start async enumeration of all installed programs.
    /// @param generation An opaque token echoed back in enumerationFinished/Failed so a driver can
    ///        drop a stale completion from a superseded (e.g. cancelled-then-restarted) run instead
    ///        of applying its results to the new one. 0 for callers that do not track generations.
    void enumerateAll(int generation = 0);

    /// @brief Synchronously enumerate ONLY UWP + provisioned packages -- no registry scan and
    ///        no icon/size enrichment (the slow whole-machine phases of enumerateAll). For a
    ///        headless caller that needs the authoritative package list (with PackageFullName)
    ///        FAST. Blocks on the two Get-AppxPackage PowerShell calls. @p scanOk is set false
    ///        if either Appx scan failed, so an empty result reads as "scan failed", never a
    ///        false "no packages installed".
    [[nodiscard]] QVector<ProgramInfo> enumerateUwpPackagesSync(bool& scanOk);

    /// @brief Synchronously enumerate ONLY Win32 registry programs (HKLM / HKCU / WOW64) --
    ///        no UWP scan and no icon/size enrichment. For a headless caller that needs the
    ///        authoritative Win32 program list (with uninstall strings) FAST. Reads the
    ///        registry directly (no external process), deduped across hives.
    [[nodiscard]] QVector<ProgramInfo> enumerateRegistryProgramsSync();

    /// @brief Request cancellation of an in-progress enumeration
    void requestCancel();

    /// @brief Reset the cancellation flag (call before starting a new enumeration)
    void resetCancel();

    /// @brief Get the last enumeration result (cached)
    [[nodiscard]] QVector<ProgramInfo> programs() const;

    /// @brief Detect orphaned entries (install path or uninstaller missing)
    void detectOrphaned(QVector<ProgramInfo>& programs);

    /// @brief Mark known bloatware using pattern database
    void markBloatware(QVector<ProgramInfo>& programs);

    /// @brief Calculate actual disk usage for a program's install directory
    // Non-static: the directory walk honors m_cancelRequested so a hostile/deep install
    // tree cannot make it run unbounded.
    [[nodiscard]] qint64 calculateDirSize(const QString& path);

Q_SIGNALS:
    void enumerationStarted();
    void enumerationProgress(int current, int total);
    /// @p generation echoes the value passed to enumerateAll (see there); a driver compares it to
    /// its current generation to ignore a stale, superseded run's completion.
    void enumerationFinished(int generation, QVector<ProgramInfo> programs);
    void enumerationFailed(int generation, const QString& error);
    void enumerationWarning(const QString& message);

private:
#ifdef Q_OS_WIN
    /// @brief Scan Win32 programs from all registry hives
    QVector<ProgramInfo> scanRegistryPrograms();

    /// @brief Scan a single registry hive's Uninstall key
    QVector<ProgramInfo> scanRegistryHive(HKEY hive,
                                          const wchar_t* subkey,
                                          ProgramInfo::Source source);
    void parseRegistryEntry(HKEY app_key,
                            ProgramInfo::Source source,
                            const QString& reg_path,
                            QVector<ProgramInfo>& results);

    /// @brief Read a single registry string value
    [[nodiscard]] static QString readRegString(HKEY key, const wchar_t* valueName);

    /// @brief Read a single registry DWORD value
    [[nodiscard]] static DWORD readRegDword(HKEY key, const wchar_t* valueName);

    /// @brief Check if entry is a system component (filter out)
    [[nodiscard]] static bool isSystemComponent(HKEY key);
#endif

    /// @brief True when an Appx/provisioned scan produced a real, parseable result. False for
    ///        process/timeout/exit failures or unparseable output, so a failed scan is never
    ///        mistaken for an empty success.
    [[nodiscard]] static bool appxScanSucceeded(const ProcessResult& result);

    /// @brief Scan UWP/AppX packages via PowerShell (scanOk reports scan success vs failure)
    QVector<ProgramInfo> scanUwpPackages(bool& scanOk);
    void parseUwpPackage(const QJsonObject& obj, QVector<ProgramInfo>& results);

    /// @brief Scan provisioned (all-users) UWP packages (scanOk reports success vs failure)
    QVector<ProgramInfo> scanProvisionedPackages(bool& scanOk);

    /// @brief Emit enumerationWarning when either Appx scan failed (fail-closed completeness)
    void warnIfAppxIncomplete(bool uwpOk, bool provisionedOk);

    /// @brief Extract icon image from executable file
    [[nodiscard]] static QImage extractIcon(const QString& path);

    /// @brief Deduplicate programs from multiple sources
    void deduplicatePrograms(QVector<ProgramInfo>& programs);

    /// @brief Resolve one program's icon in place: prefer its declared displayIcon, else the
    ///        first .exe in a LOCAL install location. @p locationIsLocal is false for remote/device
    ///        install paths, which must never be scanned (SMB auth / remote I/O).
    void resolveProgramIcon(ProgramInfo& program, bool locationIsLocal);

    /// @brief Extract icons and calculate sizes for enumerated programs
    [[nodiscard]] bool enrichWithIconsAndSizes(QVector<ProgramInfo>& programs);

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
    std::atomic<bool> m_cancelRequested{false};
    // Generation of the CURRENT enumerateAll run, echoed in enumerationFinished/Failed. Written and
    // read only on the enumeration worker thread (runs are serialized), so a plain int is safe.
    int m_generation{0};
};

// -- Compile-Time Invariants -------------------------------------------------

static_assert(std::is_base_of_v<QObject, ProgramEnumerator>,
              "ProgramEnumerator must inherit QObject.");
static_assert(!std::is_copy_constructible_v<ProgramEnumerator>,
              "ProgramEnumerator must not be copy-constructible.");

}  // namespace sak
