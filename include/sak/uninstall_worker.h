// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file uninstall_worker.h
/// @brief WorkerBase subclass for executing the uninstall pipeline

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/worker_base.h"

#include <QSet>
#include <QVector>

#include <atomic>
#include <type_traits>

namespace sak {

struct LeftoverScanReliability;

/// @brief Executes the uninstall pipeline on a background thread
///
/// Pipeline: restore point -> registry snapshot -> native uninstall ->
/// leftover scan -> report. Supports standard, forced, UWP, and
/// registry-only uninstall modes.
class UninstallWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Uninstall mode
    enum class Mode {
        Standard,         ///< Run native uninstaller + leftover scan
        ForcedUninstall,  ///< Skip native uninstaller; scan and REPORT leftovers only (no removal)
        UwpRemove,        ///< Remove UWP package via PowerShell
        RegistryOnly      ///< Only remove the registry uninstall entry (orphaned)
    };

    explicit UninstallWorker(const ProgramInfo& program,
                             Mode mode,
                             ScanLevel scanLevel,
                             bool createRestorePoint = true,
                             QObject* parent = nullptr);
    // Join the worker thread while this class's members are still alive (the base
    // ~WorkerBase runs after they are destroyed). See WorkerBase::stopAndJoin.
    ~UninstallWorker() override { stopAndJoin(); }

    UninstallWorker(const UninstallWorker&) = delete;
    UninstallWorker& operator=(const UninstallWorker&) = delete;
    UninstallWorker(UninstallWorker&&) = delete;
    UninstallWorker& operator=(UninstallWorker&&) = delete;

    /// @brief Run the native uninstaller in SILENT, BOUNDED mode (headless). When set, the
    /// Standard-mode native step uses only a fully-silent command (quietUninstallString, or a
    /// built `msiexec /x {GUID} /qn /norestart`) and a hard timeout, and REFUSES to launch a
    /// program that has no silent command -- a bare interactive uninstaller would hang a run
    /// with no user to click through it. Default (false) preserves the GUI behavior exactly:
    /// the registered uninstallString is launched and waited on indefinitely.
    void setHeadlessSilent(bool silent) { m_headlessSilent = silent; }

    /// @brief Build the fully-silent uninstall command for @p program into @p cmdOut. Returns
    /// false if none exists (no quietUninstallString and not an MSI with a product GUID) -- the
    /// signal to REFUSE a headless uninstall rather than launch an interactive one. Static so
    /// the caller can pre-check without constructing a worker. Exposed for unit testing.
    [[nodiscard]] static bool buildSilentUninstallCommand(const ProgramInfo& program,
                                                          QString& cmdOut);

    /// @brief Classify a native-uninstaller process result as success. The process exit status
    /// must be 0 and its exit code either 0 or 3010 (MSI reboot-required, a SUCCESS that asks for a
    /// restart). A cancelled run is never a success. Static + pure for unit testing.
    [[nodiscard]] static bool nativeUninstallSucceeded(int exitStatus,
                                                       int exitCode,
                                                       bool cancelled);

    /// @brief Trust screen for the program token parsed out of a registry uninstall string.
    ///
    /// The Uninstall subtree is attacker-influenceable -- ANY user can create an HKCU
    /// entry -- and the program it names is launched with the uninstaller's (usually
    /// ELEVATED) token. A bare or relative image name would be resolved by the
    /// CreateProcess search order, which includes the current directory and PATH ahead of
    /// the real install location, so a planted "setup.exe" would run elevated.
    ///
    /// Accepts ONLY a fully-qualified LOCAL path to an .exe -- "C:\\App\\unins000.exe",
    /// separators either way. REFUSES: empty/whitespace, a bare image name, any relative
    /// or drive-relative ("C:file.exe") or root-relative ("\\dir\\file.exe") form, UNC and
    /// other doubled-separator paths (an untrusted remote origin for an elevated launch),
    /// any "." or ".." segment, an alternate-data-stream suffix, an unexpanded "%VAR%",
    /// and any image whose extension is not .exe. Pure; unit-testable.
    [[nodiscard]] static bool uninstallProgramPathTrusted(const QString& exe);

Q_SIGNALS:
    /// @brief Native uninstaller has been launched
    void nativeUninstallerStarted(const QString& programName);

    /// @brief Native uninstaller completed
    void nativeUninstallerFinished(int exitCode);

    /// @brief Registry snapshot captured (before state)
    void registrySnapshotCaptured();

    /// @brief Restore point created
    void restorePointCreated(const QString& name);

    /// @brief Leftover scan started
    void leftoverScanStarted(sak::ScanLevel level);

    /// @brief Leftover scan progress
    void leftoverScanProgress(const QString& currentPath, int found);

    /// @brief Leftover scan complete
    void leftoverScanFinished(QVector<sak::LeftoverItem> leftovers);

    /// @brief Full uninstall pipeline complete
    void uninstallComplete(sak::UninstallReport report);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    ProgramInfo m_program;
    Mode m_mode;
    ScanLevel m_scanLevel;
    bool m_createRestorePoint{true};
    // Atomic: setHeadlessSilent() runs on the caller thread while runNativeUninstaller() reads
    // this on the worker thread. A plain bool would be a data race that could non-atomically
    // select bounded-silent vs. unbounded-interactive execution.
    std::atomic<bool> m_headlessSilent{false};

    // Pipeline stages
    [[nodiscard]] bool createRestorePoint();
    [[nodiscard]] bool captureRegistrySnapshot();
    [[nodiscard]] bool runNativeUninstaller(int& exitCode);
    /// @param reliabilityOut Records whether each Advanced (system-object/registry) leftover
    ///        phase actually completed, so a FAILED enumeration is not misread as "none found".
    [[nodiscard]] QVector<LeftoverItem> scanLeftovers(LeftoverScanReliability& reliabilityOut);
    [[nodiscard]] bool removeUwpPackage();
    [[nodiscard]] bool removeRegistryEntry();

    /// @brief Create the requested restore point. FAIL CLOSED: returns an error (aborting the
    /// uninstall) if the user asked for a restore point and it could not be created.
    [[nodiscard]] std::expected<void, sak::error_code> runRestorePointPhase(
        UninstallReport& report);

    /// @brief Scan for leftovers and emit the terminal completion. Returns operation_cancelled
    /// (before emitting any terminal success) if a stop was requested during the scan.
    [[nodiscard]] std::expected<void, sak::error_code> runLeftoverPhase(UninstallReport& report);

    /// @brief Capture the before-snapshot; on empty/failed capture record a degraded-detection
    /// warning in the report instead of signalling a snapshot that was never captured.
    void captureSnapshotOrWarn(UninstallReport& report);

    /// @brief Execute the standard uninstall flow (registry snapshot + native uninstaller)
    [[nodiscard]] std::expected<void, sak::error_code> executeStandardMode(UninstallReport& report);

    /// @brief Execute UWP package removal (complete + early return)
    [[nodiscard]] std::expected<void, sak::error_code> executeUwpMode(UninstallReport& report);

    /// @brief Execute registry-only cleanup (complete + early return)
    [[nodiscard]] std::expected<void, sak::error_code> executeRegistryMode(UninstallReport& report);

    // Registry snapshot data
    QSet<QString> m_registrySnapshotBefore;

    /// @brief Stop flag passed to LeftoverScanner (bridges WorkerBase::stopRequested)
    std::atomic<bool> m_scanStopFlag{false};

    // Helpers
    [[nodiscard]] bool isMsiInstaller() const;
    [[nodiscard]] QString buildMsiUninstallCommand() const;
    [[nodiscard]] QString extractGuidFromUninstallString() const;
};

// -- Compile-Time Invariants -------------------------------------------------

static_assert(std::is_base_of_v<WorkerBase, UninstallWorker>,
              "UninstallWorker must inherit WorkerBase.");
static_assert(!std::is_copy_constructible_v<UninstallWorker>,
              "UninstallWorker must not be copy-constructible.");

}  // namespace sak

Q_DECLARE_METATYPE(sak::UninstallWorker::Mode)
