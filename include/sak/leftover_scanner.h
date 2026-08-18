// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file leftover_scanner.h
/// @brief Multi-level leftover scanning for orphaned files, folders, and
///        registry entries after program uninstallation

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QPair>
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

/// @brief Per-phase reliability for the Advanced (system-object) leftover phases.
///
/// scanServices / scanScheduledTasks / scanFirewallRules each shell out (sc / schtasks / netsh);
/// scanStartupEntries reads the registry Run keys. A nonzero exit, a spawn block (AppLocker/SRP),
/// the phase timeout, or a registry read failure (e.g. ERROR_ACCESS_DENIED) yields an EMPTY item
/// vector that is indistinguishable from an honest "nothing found" -- the endemic fail-open. A
/// caller passes this to scan() so a FAILED enumeration surfaces as scan_complete=false instead of
/// a dishonest empty-success. Moderate scans (no system-object phases) leave every flag true.
struct LeftoverScanReliability {
    bool services_ok{true};         ///< sc.exe query succeeded
    bool scheduled_tasks_ok{true};  ///< schtasks.exe /query succeeded
    bool firewall_ok{true};         ///< netsh advfirewall show rule succeeded (both directions)
    bool startup_ok{true};   ///< Run-key registry reads succeeded (absent key is NOT failure)
    bool registry_ok{true};  ///< post-uninstall registry snapshot fully enumerated (not partial)
    [[nodiscard]] bool allOk() const {
        return services_ok && scheduled_tasks_ok && firewall_ok && startup_ok && registry_ok;
    }
};

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

    /// @param reliability Optional out-param. When non-null and the scan is Advanced, its flags
    ///        record whether each shell-out system-object phase ACTUALLY completed (vs failed and
    ///        returned empty). Null (the GUI default) preserves the original behavior exactly.
    [[nodiscard]] QVector<LeftoverItem> scan(
        const std::atomic<bool>& stopRequested,
        std::function<void(const QString&, int)> progressCallback = {},
        LeftoverScanReliability* reliability = nullptr);

    /// @brief Syntactic screen for a registry-supplied InstallLocation, BEFORE that value
    ///        is allowed to name anything a cleanup deletes.
    ///
    /// InstallLocation lives in the Uninstall subtree, which any user can write under
    /// HKCU, yet a whole-directory leftover built from it is handed to a recursive delete
    /// run by an administrator. The value must therefore pin exactly one literal local
    /// directory: shares the windows_path_policy dialect with the sibling UninstallString
    /// screen, so a relative, drive-relative, UNC, traversal-bearing, "%VAR%"-carrying or
    /// volume-root value is refused, as is any directory in @p criticalRoots (the Windows
    /// / Program Files / ProgramData / AppData / TEMP roots, the profiles container and
    /// every user profile root -- see criticalInstallRoots()).
    ///
    /// @return an empty string when the value may proceed to the filesystem and ownership
    ///         checks, otherwise a human-readable refusal reason. Pure; unit-testable.
    [[nodiscard]] static QString installLocationSyntaxRefusal(const QString& rawLocation,
                                                              const QStringList& criticalRoots);

    /// @brief Proof that @p canonicalLocation really is THIS program's directory rather
    ///        than an arbitrary path a registry value merely claims is.
    ///
    /// Derived ONLY from the program's own identity -- never from the install location
    /// itself, which would be circular. The directory leaf, or the "Publisher\\Product"
    /// pair formed with its parent, must fold to @p displayName or @p packageFamilyName
    /// once case and non-alphanumerics are removed (installer display names routinely
    /// carry a version/architecture suffix the directory omits, and vice versa). Pure;
    /// unit-testable.
    [[nodiscard]] static bool installLocationMatchesProgram(const QString& canonicalLocation,
                                                            const QString& displayName,
                                                            const QString& packageFamilyName);

    /// @brief Directories that must NEVER become a deletion target however a registry
    ///        value names them. Read from the live environment (so a machine whose system
    ///        drive is not C: is protected too) plus every user profile root present on
    ///        the machine. Volume roots need no entry: the syntactic screen already
    ///        refuses them.
    [[nodiscard]] static QStringList criticalInstallRoots();

    /// @brief True when @p path's LEAF directory name is a shared OS/vendor container
    /// (windows / system32 / programdata / program files / ...), matched case-insensitively on
    /// ANY drive. This is the DRIVE-AGNOSTIC half of the risk classifier: kProtectedPaths
    /// hard-codes C:, but this leaf match keeps a non-C: system directory (e.g.
    /// D:\Windows\System32) classified Risky just like C:'s, so the leftover cleaner is safe on a
    /// machine whose system drive is not C:.
    [[nodiscard]] static bool isSharedContainerPath(const QString& path);

private:
    /// @brief Outcome of screening the registry-supplied InstallLocation.
    struct ScreenedInstallLocation {
        /// Native, separator-normalized location that passed the SYNTACTIC screen only.
        /// Safe to derive matching NAMES from and to stat; empty when even the syntax was
        /// refused. Carries no deletion authority on its own.
        QString syntactic;
        /// Canonical native location that MAY authorize deletion: additionally exists, is
        /// a directory, is no reparse point, and is proven to belong to this program.
        QString trusted;
        /// Why `trusted` is empty. Empty when the location was accepted, and also when the
        /// program records no install location at all (nothing to refuse).
        QString refusal;
    };

    ProgramInfo m_program;
    ScanLevel m_level;
    QSet<QString> m_registryBefore;

    /// Syntactically-screened install location (see ScreenedInstallLocation::syntactic).
    QString m_screenedInstallLocation;
    /// Install location that may authorize deletion (ScreenedInstallLocation::trusted).
    /// EVERY use of the install location as authority reads THIS, never m_program.
    QString m_trustedInstallLocation;
    /// Refusal reason shown on the report-only install-location item.
    QString m_installLocationRefusal;

    QStringList m_exactNames;     ///< Exact names for directory/file matching (lowercase)
    QString m_installDirName;     ///< Last component of install path (lowercase)
    QString m_installParentName;  ///< Parent dir of install path (lowercase)
    QString m_concatenatedName;   ///< Display name without separators (lowercase)

    void buildExactNames();

    /// @brief Run both screening tiers over @p program's recorded install location.
    [[nodiscard]] static ScreenedInstallLocation screenInstallLocation(const ProgramInfo& program);

    /// @brief Filesystem + ownership tier. Returns the canonical location, or an empty
    ///        string with @p refusalOut set to the reason it may not authorize a delete.
    [[nodiscard]] static QString provenInstallLocation(const QString& screenedLocation,
                                                       const ProgramInfo& program,
                                                       QString& refusalOut);

    /// @brief Visibility-only item for an install location that passed the syntax screen
    ///        but could not be proven to belong to this program. Empty when there is
    ///        nothing safe to report.
    [[nodiscard]] QVector<LeftoverItem> reportOnlyInstallLocation() const;

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
    QVector<LeftoverItem> scanRegistryDiff(const std::atomic<bool>& stopRequested, bool& ok);
    QVector<LeftoverItem> scanKnownRegistryPaths(const std::atomic<bool>& stopRequested);
    [[nodiscard]] bool registryKeyMatchesProgram(const QString& keyPath) const;
#endif

    // Phase 4: System objects (Advanced only). The shell-out phases take an out bool& set FALSE if
    // the underlying process failed (so an empty result is not misread as an honest "none found").
    QVector<LeftoverItem> scanServices(const std::atomic<bool>& stopRequested, bool& ok);
    QVector<LeftoverItem> scanScheduledTasks(const std::atomic<bool>& stopRequested, bool& ok);
    QVector<LeftoverItem> scanFirewallRules(const std::atomic<bool>& stopRequested, bool& ok);
    QVector<LeftoverItem> scanStartupEntries(const std::atomic<bool>& stopRequested, bool& ok);

    void scanFirewallDirection(const QStringList& netsh_args,
                               const QString& description,
                               const std::atomic<bool>& stopRequested,
                               QVector<LeftoverItem>& items,
                               bool& ok);
    /// Append a FirewallRule item for a matched "Rule Name:" header line. Returns the
    /// new item's index in @p items, or -1 when the rule name does not match this
    /// program (so the caller ignores that rule's subsequent field lines).
    [[nodiscard]] int appendFirewallRule(const QString& headerLine,
                                         const QString& description,
                                         QVector<LeftoverItem>& items) const;
#ifdef Q_OS_WIN
    /// Read one Run key. Returns TRUE if the enumeration was reliable (key read OK, or the key was
    /// simply absent -- an honest "no entries"), FALSE on a real read failure (e.g.
    /// ERROR_ACCESS_DENIED) so the caller can flag the startup phase incomplete.
    [[nodiscard]] bool scanRunKey(HKEY hive,
                                  const wchar_t* subkey,
                                  const QString& hive_name,
                                  const std::atomic<bool>& stopRequested,
                                  QVector<LeftoverItem>& items);
    /// Match + append a single Run-key value (nesting/complexity split out of scanRunKey). Returns
    /// TRUE when the value was READ reliably (whether or not it matched this program), FALSE when
    /// the read itself failed (a genuine error or the grow ceiling) so the caller can flag the
    /// whole startup phase incomplete instead of silently dropping the entry and reporting "none
    /// here".
    [[nodiscard]] bool appendRunKeyValue(HKEY key,
                                         DWORD index,
                                         const QString& hive_name,
                                         const wchar_t* subkey,
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
    /// @brief Sum the sizes of all files under @p path. Polls @p stopRequested inside the walk (and
    ///        caps the entry count) so a cooperative cancel / deadline actually interrupts a huge
    ///        subtree, rather than running to completion after the outer scan was told to stop.
    [[nodiscard]] static qint64 calculateSize(const QString& path,
                                              const std::atomic<bool>& stopRequested);
};

/// @brief Build service leftover items from an enumerated (serviceName, displayName) list.
///
/// Keeps only the services the @p matches predicate accepts (applied to BOTH the key name and the
/// display name), tagging each as a Risky Service leftover. Polls @p stopRequested so a cooperative
/// cancel interrupts the walk. This is the locale-independent successor to the old sc.exe console
/// parse: the (name, display) pairs come from the SCM API (EnumServicesStatusExW), whose fields are
/// language-neutral, so it no longer keys off English "SERVICE_NAME:"/"DISPLAY_NAME:" labels that
/// were empty on non-English Windows. Pure + unit-testable (no SCM handle required).
[[nodiscard]] QVector<LeftoverItem> buildServiceLeftoverItems(
    const QVector<QPair<QString, QString>>& services,
    const std::function<bool(const QString&)>& matches,
    const std::atomic<bool>& stopRequested);

// -- Compile-Time Invariants -------------------------------------------------

static_assert(!std::is_copy_constructible_v<LeftoverScanner>,
              "LeftoverScanner must not be copy-constructible.");

}  // namespace sak
