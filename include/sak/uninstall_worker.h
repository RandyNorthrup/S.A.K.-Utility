// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file uninstall_worker.h
/// @brief WorkerBase subclass for executing the uninstall pipeline

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/worker_base.h"

#include <QSet>
#include <QVector>

#include <type_traits>

namespace sak {

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
        ForcedUninstall,  ///< Skip native uninstaller, scan + remove all traces
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
    bool m_headlessSilent{false};

    // Pipeline stages
    [[nodiscard]] bool createRestorePoint();
    [[nodiscard]] bool captureRegistrySnapshot();
    [[nodiscard]] bool runNativeUninstaller();
    [[nodiscard]] QVector<LeftoverItem> scanLeftovers();
    [[nodiscard]] bool removeUwpPackage();
    [[nodiscard]] bool removeRegistryEntry();

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
